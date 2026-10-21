/*
 * servers/hwdb/server.c -- hwdb, the first server (docs §9.4, §11.4-§11.6)
 *
 * The kernel starts it with the device root (its second argument). It
 * reads the firmware's description where it lies (MapFirmware), builds
 * the graph, and starts each unit's driver once all its devices are
 * ready: creates the process held, grants it the unit's leases, starts
 * it, and answers its HWDB_BIND and HWDB_STARTED (planar/drv.h). Then it
 * watches:
 *
 * - A unit that dies is torn down the one way there is (§11.4): the
 *   kernel has masked its lines and quarantined its DMA; hwdb checks it
 *   did, runs the stop method, revokes the leases (which frees the
 *   quarantine), and starts it again.
 * - A death hwdb didn't ask for is a failure: restarted after a backoff
 *   that doubles, and given up on at FAILS within WINDOW_MS (VQ23). Its
 *   devices fail, so their consumers stay waiting.
 *
 * hwdb never waits for a driver (§11.6 D6): one task, one loop, every
 * message answered as it comes.
 */
#include <planar/dos.h>
#include <planar/syscalls.h>

#include "server.h"

constexpr int64_t SIG_PORT = 16;        /* a driver at hwdb's port */
constexpr int64_t SIG_TIMER = 19;
constexpr unsigned CONNS = 64;
constexpr uint64_t ARENA = 4 << 20;
constexpr unsigned TABLES = 64;

struct hwdb hwdb;

static struct MsgPort conns[CONNS];
static uint32_t conn_unit[CONNS];       /* UINT32_MAX until it binds */
static unsigned nconns;

/* --- Saying things. --- */

void say(const char *text)
{
    KLog(text);
}

const char *unit_driver(const struct unit *u)
{
    return hwdb.g.drivers[hwdb.g.leases[u - hwdb.units].driver].name;
}

/* The unit's first device's path, and how many more. */
const char *unit_path(const struct unit *u, struct hw_line *l)
{
    const struct hw_lease *ls = &hwdb.g.leases[u - hwdb.units];

    hw_put_path(l, &hwdb.g, ls->nodes[0]);
    if (ls->nnodes > 1) {
        hw_put(l, " and ");
        hw_put_dec(l, ls->nnodes - 1);
        hw_put(l, " more");
    }
    return l->text;
}

void say_unit(const struct unit *u, const char *text)
{
    struct hw_line l = { 0 };

    hw_put(&l, "hwdb: ");
    hw_put(&l, unit_driver(u));
    hw_put(&l, " (");
    unit_path(u, &l);
    hw_put(&l, "): ");
    hw_put(&l, text);
    say(l.text);
}

void check_failed(const struct unit *u, const char *what)
{
    struct hw_line l = { 0 };

    hw_put(&l, "check failed: ");
    hw_put(&l, what);
    say_unit(u, l.text);
    hwdb.errors++;
}

/* --- The firmware's description. --- */

static uint64_t rd(const uint8_t *p, unsigned off, unsigned bytes)
{
    uint64_t v = 0;

    for (unsigned i = 0; i < bytes; i++)
        v |= (uint64_t)p[off + i] << (8 * i);
    return v;
}

/* What MapFirmware mapped, for acpid's windows (acpid.c): the RSDP, the
 * root table and what it lists, and the FADT's DSDT and FACS. */
static void tables_mapped(const uint8_t *rsdp, const struct hw_table *t, unsigned n)
{
    struct acpid *a = &acpid;

    a->tables[a->ntables++] = (typeof(a->tables[0])){ (uint64_t)rsdp, rsdp[15] >= 2 ? rd(rsdp, 20, 4) : 20 };
    for (unsigned i = 0; i < n && a->ntables < ACPID_TABLES_MAX; i++) {
        const uint8_t *p = t[i].data;

        a->tables[a->ntables++] = (typeof(a->tables[0])){ (uint64_t)p, t[i].len };
        if (p[0] == 'F' && p[1] == 'A' && p[2] == 'C' && p[3] == 'P' && t[i].len >= 44) {
            uint64_t dsdt = t[i].len >= 148 && rd(p, 140, 8) ? rd(p, 140, 8) : rd(p, 40, 4);
            uint64_t facs = t[i].len >= 140 && rd(p, 132, 8) ? rd(p, 132, 8) : rd(p, 36, 4);

            for (unsigned k = 0; k < 2 && a->ntables < ACPID_TABLES_MAX; k++) {
                const uint8_t *q = (const uint8_t *)(k ? facs : dsdt);

                if (q)
                    a->tables[a->ntables++] = (typeof(a->tables[0])){ (uint64_t)q, rd(q, 4, 4) };
            }
        }
    }
}

/*
 * The framebuffer UEFI's GOP left (MapFirmware): a node of its own, as a
 * device tree's simple-framebuffer would be, its geometry in its
 * registers' cells, for display (step 7). Neither description has it.
 */
static bool framebuffer(void)
{
    const struct FirmwareInfo *fi = &hwdb.fb;
    struct hw_line l = { 0 };
    struct hw_res *r;
    hw_id n;

    if (!fi->fi_FbBase || !fi->fi_FbSize)
        return true;
    hw_put(&l, "framebuffer@");
    hw_put_hex(&l, fi->fi_FbBase);
    if ((n = hw_node_new(&hwdb.g, 0, hw_strdup(&hwdb.g, l.text + 0, l.len))) == HW_NONE ||
        !hw_key_add(&hwdb.g, n, "simple-framebuffer") || !(r = hw_res_add(&hwdb.g, n)))
        return !hwdb.g.full;
    r->kind = HW_MMIO;
    r->base = fi->fi_FbBase;
    r->size = fi->fi_FbSize;
    r->ncells = 4;
    r->cells[0] = fi->fi_FbWidth;
    r->cells[1] = fi->fi_FbHeight;
    r->cells[2] = fi->fi_FbStride;
    r->cells[3] = fi->fi_FbFormat;
    return true;
}

static bool build(void)
{
    struct FirmwareInfo fi;
    void *space;

    if (AllocArea(ARENA, AREAF_CLEAR, &space) < 0)
        return false;
    hwdb.arena = (struct hw_arena){ space, ARENA, 0 };
    hw_graph_init(&hwdb.g, &hwdb.arena, 4096);
    if (MapFirmware(hwdb.root, &fi) != 0)
        return false;
    hwdb.fb = fi;
    if (fi.fi_Dtb)
        return hw_from_fdt(&hwdb.g, (const void *)fi.fi_Dtb, (uint32_t)fi.fi_DtbSize) &&
               framebuffer();
    if (fi.fi_Rsdp) {
        static struct hw_table t[TABLES];
        const uint8_t *rsdp = (const uint8_t *)fi.fi_Rsdp, *root;
        unsigned n = 0, entry;
        uint32_t len;
        uint64_t at = 0;

        /* The XSDT (ACPI 2.0 on), else the RSDT; then what it lists. */
        if (rsdp[15] >= 2)
            for (unsigned i = 0; i < 8; i++)
                at |= (uint64_t)rsdp[24 + i] << (8 * i);
        entry = at ? 8 : 4;
        if (!at)
            for (unsigned i = 0; i < 4; i++)
                at |= (uint64_t)rsdp[16 + i] << (8 * i);
        if (!at)
            return false;
        root = (const uint8_t *)at;
        len = (uint32_t)root[4] | root[5] << 8 | root[6] << 16 | (uint32_t)root[7] << 24;
        t[n++] = (struct hw_table){ root, len };
        hwdb.rsdp = fi.fi_Rsdp;
        for (uint32_t off = 36; off + entry <= len && n < TABLES; off += entry) {
            const uint8_t *tb;

            at = 0;
            for (unsigned i = 0; i < entry; i++)
                at |= (uint64_t)root[off + i] << (8 * i);
            if (!at)
                continue;
            tb = (const uint8_t *)at;
            t[n++] = (struct hw_table){
                tb, (uint32_t)tb[4] | tb[5] << 8 | tb[6] << 16 | (uint32_t)tb[7] << 24
            };
        }
        tables_mapped(rsdp, t, n);
        return hw_from_acpi(&hwdb.g, t, n) && framebuffer();
    }
    return false;
}

/*
 * Each SMMUv3 (Arm IHI 0070, §6.3): nothing drives one in V4, so what
 * matters is what the firmware left it doing. Disabled (CR0.SMMUEN 0)
 * with GBPA.ABORT clear, it passes DMA through untranslated: then it's
 * the firmware's, and the devices behind it may bind. Anything else and
 * they wait on it, named (pci.c gives each function the reference).
 */
static void smmus(void)
{
    for (hw_id i = 1; i < hwdb.g.count; i++) {
        struct hw_node *n = &hwdb.g.nodes[i];
        struct hw_line l = { 0 };
        uint32_t cr0, gbpa;
        int64_t h;
        void *at;

        if (!hw_has_key(n, "arm,smmu-v3") || !n->enabled || n->nres == 0 ||
            n->res[0].kind != HW_MMIO || n->res[0].size < 0x1000)
            continue;
        if ((h = CreateLease(hwdb.root, n->res[0].base & ~0xfffULL, 0x1000, nullptr, 0)) < 0)
            continue;
        if (MapLease((Handle)h, &at) == 0) {
            volatile uint32_t *r = (volatile uint32_t *)(n->res[0].base & ~0xfffULL);

            cr0 = r[0x20 / 4];
            gbpa = r[0x44 / 4];
            hw_put(&l, "hwdb: ");
            hw_put_path(&l, &hwdb.g, i);
            if (!(cr0 & 1) && !(gbpa & 1u << 20)) {
                n->state = HW_S_FIRMWARE;
                n->why = "left passing DMA through by the firmware";
                hw_put(&l, ": off, passing DMA through");
            } else {
                hw_put(&l, cr0 & 1 ? ": translating" : ": off, aborting DMA");
                hw_put(&l, "; nothing behind it can bind");
            }
            say(l.text);
        }
        CloseHandle((Handle)h);
    }
}

/* --- Units. --- */

static struct hw_lease *lease_of(const struct unit *u)
{
    return &hwdb.g.leases[u - hwdb.units];
}

void unit_fail(struct unit *u, const char *why)
{
    const struct hw_lease *ls = lease_of(u);

    for (unsigned i = 0; i < ls->nnodes; i++)
        if (hwdb.g.nodes[ls->nodes[i]].state != HW_S_DECLINED)
            hw_unbind(&hwdb.g, ls->nodes[i], HW_S_FAILED, why);
    u->state = U_DONE;
}

bool hwdb_busy(void)
{
    if (hwdb.test && acpid.proc && !acpid.probed)
        return true;                /* its probe isn't over */
    for (uint32_t i = 0; i < hwdb.g.nleases; i++) {
        const struct unit *u = &hwdb.units[i];

        if (u->state == U_DUE || u->state == U_STARTING || u->state == U_POWERING ||
            (u->state == U_RUNNING && u->accepted == 0))    /* declined all: ending */
            return true;
    }
    return false;
}

void unit_launch(struct unit *u)
{
    const struct hw_lease *ls = lease_of(u);
    int64_t p;

    u->gen++;
    p = CreateProcess(unit_driver(u), (uint32_t)(u - hwdb.units) | u->gen << 12, SIG_DEAD,
                      PROCF_HELD);
    if (p == ERR_NOTFOUND) {
        unit_fail(u, "its program isn't built yet");
        return;
    }
    if (p < 0) {
        unit_fail(u, "its process couldn't be made");
        return;
    }
    for (unsigned r = 0; r < ls->nranges; r++) {
        int64_t h = u->lease[r];

        if (!h) {
            h = CreateLease(hwdb.root, ls->ranges[r].base, ls->ranges[r].size,
                            r == 0 ? ls->lines : nullptr, r == 0 ? ls->nlines : 0);
            if (h > 0 && ls->dma_limit && LimitLeaseDma((Handle)h, ls->dma_limit) != 0) {
                CloseHandle((Handle)h);
                h = ERR_BADARG;
            }
            if (h > 0 && r == 0 && ls->call && SetLeaseCall((Handle)h, ls->call) != 0) {
                CloseHandle((Handle)h);
                h = ERR_BADARG;
            }
        }
        if (h < 0 || GrantLease((Handle)h, (Handle)p) != 0) {
            CloseHandle((Handle)p);
            unit_fail(u, h < 0 ? "the kernel refused its lease" : "its lease couldn't be granted");
            return;
        }
        u->lease[r] = (Handle)h;
    }
    u->dma = UINT64_MAX;
    if (StartProcess((Handle)p) != 0) {
        CloseHandle((Handle)p);
        unit_fail(u, "its process couldn't be started");
        return;
    }
    u->proc = (Handle)p;
    u->accepted = 0;
    u->state = U_STARTING;
    u->started = hwdb.now;
    u->due = hwdb.now + START_MS * MS;
    u->starts++;
}

/* Power before lease (D3): a unit whose devices need power's waits for
 * its vote (power.c), then is launched. */
static void start(struct unit *u)
{
    if (power_needed(u))
        power_vote(u);
    else
        unit_launch(u);
}

/* Killed running, a driver's DMA Areas are what it allocated in Bind:
 * all of them must be in quarantine after. */
void unit_kill(struct unit *u)
{
    struct LeaseInfo li;

    if (!u->proc)
        return;
    u->dma = UINT64_MAX;
    if (u->state == U_RUNNING && QueryLease(u->lease[0], &li) == 0)
        u->dma = li.li_DmaPages;
    u->killing = true;
    KillProcess(u->proc);
}

void unit_retry(struct unit *u)
{
    const struct hw_lease *ls = lease_of(u);

    for (unsigned i = 0; i < ls->nnodes; i++)
        if (hwdb.g.nodes[ls->nodes[i]].state == HW_S_FAILED)
            hw_unbind(&hwdb.g, ls->nodes[i], HW_S_READY, nullptr);
    u->nfails = 0;
    u->state = U_DUE;
    u->due = hwdb.now;
}

/* A node is ready: hwdb's own bind at once; a unit starts with its last. */
static void ready(hw_id id)
{
    struct hw_node *n = &hwdb.g.nodes[id];
    struct unit *u;

    if (hwdb.g.drivers[n->driver].kind == HW_DRV_HWDB) {
        hw_bound(&hwdb.g, id);
        return;
    }
    if (n->lease == HW_NOLEASE) {
        hw_unbind(&hwdb.g, id, HW_S_FAILED, "it has no registers to lease");
        return;
    }
    u = &hwdb.units[n->lease];
    if (++u->ready == lease_of(u)->nnodes && u->state == U_WAITING)
        start(u);
}

static void pump(void)
{
    if (hwdb.test && acpid.proc && !acpid.probed)
        return;                     /* no driver holds a lease while acpid probes */
    for (;;) {
        hw_id n;

        while ((n = hw_next(&hwdb.g)) != HW_NONE)
            ready(n);
        if (hwdb_busy() || !hw_unstick(&hwdb.g))
            return;
    }
}

/* --- Stop methods (§11.4). --- */

/*
 * virtio's is its status register: writing 0 resets the device, which
 * then touches no more memory (virtio 1.2 §4.2.2.1). The unit's
 * registers are hwdb's to map while it's stopping.
 */
static bool stop_virtio(struct unit *u)
{
    const struct hw_lease *ls = lease_of(u);

    for (unsigned r = 0; r < ls->nranges; r++) {
        void *at;
        int64_t e = MapLease(u->lease[r], &at);

        if (e != 0 && e != ERR_EXISTS)
            return false;
    }
    for (unsigned i = 0; i < ls->nnodes; i++) {
        const struct hw_node *n = &hwdb.g.nodes[ls->nodes[i]];
        volatile uint32_t *regs;
        unsigned spins = 0;

        if (n->nres == 0 || n->res[0].kind != HW_MMIO || n->res[0].size < 0x100)
            continue;
        regs = (volatile uint32_t *)n->res[0].base;
        if (regs[0] != 0x74726976)
            continue;
        regs[0x70 / 4] = 0;
        while (regs[0x70 / 4] != 0)
            if (++spins > 1000000)
                return false;
    }
    return true;
}

/* NVMe's is clearing CC.EN, which resets the controller: once CSTS.RDY
 * is clear it has stopped processing commands (NVMe 2.0 §3.7.2). */
static bool stop_nvme(struct unit *u)
{
    const struct hw_lease *ls = lease_of(u);

    for (unsigned r = 0; r < ls->nranges; r++) {
        void *at;
        int64_t e = MapLease(u->lease[r], &at);

        if (e != 0 && e != ERR_EXISTS)
            return false;
    }
    for (unsigned i = 0; i < ls->nnodes; i++) {
        const struct hw_node *n = &hwdb.g.nodes[ls->nodes[i]];
        volatile uint32_t *regs;
        uint64_t until = CurrentTime() + 10000 * MS;

        if (n->nres == 0 || n->res[0].kind != HW_MMIO || n->res[0].size < 0x2000)
            continue;
        regs = (volatile uint32_t *)n->res[0].base;
        regs[0x14 / 4] = 0;
        while (regs[0x1c / 4] & 1)
            if (regs[0x1c / 4] == ~0u || CurrentTime() > until)
                return false;
    }
    return true;
}

/* SDHCI's is Software Reset For All, which stops its DMA (SD Host
 * Controller 3.00 §2.2.16). */
static bool stop_sdhci(struct unit *u)
{
    const struct hw_lease *ls = lease_of(u);

    for (unsigned r = 0; r < ls->nranges; r++) {
        void *at;
        int64_t e = MapLease(u->lease[r], &at);

        if (e != 0 && e != ERR_EXISTS)
            return false;
    }
    for (unsigned i = 0; i < ls->nnodes; i++) {
        const struct hw_node *n = &hwdb.g.nodes[ls->nodes[i]];
        volatile uint8_t *regs;
        uint64_t until = CurrentTime() + 1000 * MS;

        if (n->nres == 0 || n->res[0].kind != HW_MMIO || n->res[0].size < 0x100)
            continue;
        regs = (volatile uint8_t *)n->res[0].base;
        regs[0x2f] = 1;
        while (regs[0x2f] & 1)
            if (CurrentTime() > until)
                return false;
    }
    return true;
}

/* True if the device can't reach memory any more. */
static bool stop(struct unit *u, uint64_t quarantined)
{
    const char *name = unit_driver(u);

    if (hw_streq(name, "virtio-mmio"))
        return stop_virtio(u);
    if (hw_streq(name, "nvme"))
        return stop_nvme(u);
    if (hw_streq(name, "sdhci"))
        return stop_sdhci(u);
    if (hw_streq(name, "display"))
        return stop_display(u);
    if (hw_streq(name, "xhci"))
        return stop_xhci(u);
    if (hw_streq(name, "gpu-panthor"))
        return stop_gpu(u);
    return quarantined == 0;    /* none: only a device that kept no DMA is safe (VQ24) */
}

/* --- Deaths. --- */

static void close_conn(uint32_t idx)
{
    for (unsigned c = 0; c < nconns; c++)
        if (conn_unit[c] == idx) {
            ClosePort(&conns[c]);
            conns[c] = conns[--nconns];
            conn_unit[c] = conn_unit[nconns];
            return;
        }
}

static void died(struct unit *u, const struct ProcessInfo *pi)
{
    const struct hw_lease *ls = lease_of(u);
    uint64_t quarantined = 0;
    struct LeaseInfo li;
    bool deliberate = u->killing && !u->late;

    u->deaths++;
    /* What the kernel did: lines masked and unbound, DMA quarantined. */
    for (unsigned r = 0; r < ls->nranges; r++) {
        if (QueryLease(u->lease[r], &li) != 0 || li.li_State != LEASES_STOPPING)
            check_failed(u, "its lease isn't stopping");
        else if (li.li_DmaPages != 0)
            check_failed(u, "DMA Areas outlived it, unquarantined");
        quarantined += li.li_QuarantinedPages;
    }
    if (u->killing && u->dma != UINT64_MAX && quarantined != u->dma)
        check_failed(u, "its DMA Areas weren't all quarantined");
    hwdb.quarantined += quarantined;
    close_conn((uint32_t)(u - hwdb.units));
    CloseHandle(u->proc);
    u->proc = 0;

    if (!stop(u, quarantined)) {
        /* No stop method: its quarantine stays, and so do its leases. */
        say_unit(u, "no way to stop it, and it had DMA: its memory stays quarantined");
        unit_fail(u, "it died with DMA, and nothing can stop it");
        return;
    }
    for (unsigned r = 0; r < ls->nranges; r++)
        if (RevokeLease(u->lease[r]) != 0 || QueryLease(u->lease[r], &li) != 0 ||
            li.li_State != LEASES_IDLE || li.li_QuarantinedPages != 0)
            check_failed(u, "revoking its lease left it held or quarantined");
    power_release(u);

    for (unsigned i = 0; i < ls->nnodes; i++)
        if (hwdb.g.nodes[ls->nodes[i]].state == HW_S_BOUND)
            hw_unbind(&hwdb.g, ls->nodes[i], HW_S_READY, nullptr);

    if (hwdb.stopping) {
        u->state = U_DONE;
    } else if (!u->killing && pi->pi_State == PROCS_EXITED && pi->pi_ExitCode == 0 &&
               u->accepted == 0 && u->state == U_RUNNING) {
        u->state = U_DONE;          /* it declined every device */
    } else if (deliberate) {
        u->state = U_DUE;
        u->due = hwdb.now;
    } else {
        unsigned recent = 0;

        if (hwdb.test && !(u->flags & DRVF_CRASH)) {
            struct hw_line l = { 0 };

            hw_put(&l, "it died unasked: ");
            if (pi->pi_State == PROCS_EXITED) {
                hw_put(&l, "exit ");
                if (pi->pi_ExitCode < 0)
                    hw_put(&l, "-");
                hw_put_dec(&l, pi->pi_ExitCode < 0 ? (uint64_t)-pi->pi_ExitCode
                                                   : (uint64_t)pi->pi_ExitCode);
            } else {
                hw_put(&l, u->late ? "too slow to start" : "fault ");
                if (!u->late)
                    hw_put_dec(&l, pi->pi_Fault);
            }
            check_failed(u, l.text);
        }

        if (u->nfails == FAILS) {
            for (unsigned i = 1; i < FAILS; i++)
                u->fail_at[i - 1] = u->fail_at[i];
            u->nfails--;
        }
        u->fail_at[u->nfails++] = hwdb.now;
        for (unsigned i = 0; i < u->nfails; i++)
            recent += hwdb.now - u->fail_at[i] < WINDOW_MS * MS;
        if (recent >= FAILS) {
            say_unit(u, "failed 5 times in 10 s: given up");
            unit_fail(u, "it failed 5 times in 10 s");
        } else {
            u->state = U_DUE;
            u->due = hwdb.now + (BACKOFF_MS << (recent - 1)) * MS;
            if (!hwdb.test)
                say_unit(u, u->late ? "didn't start in time: restarting" : "died: restarting");
        }
    }
    u->killing = u->late = false;
}

static void reap(void)
{
    for (uint32_t i = 0; i < hwdb.g.nleases; i++) {
        struct unit *u = &hwdb.units[i];
        struct ProcessInfo pi;

        if (u->proc && QueryProcess(u->proc, &pi) == 0 && pi.pi_State >= PROCS_EXITED)
            died(u, &pi);
    }
}

/* --- The protocol. --- */

static struct unit *unit_of(uint32_t token)
{
    uint32_t i = token & 0xfff;

    if (i >= hwdb.g.nleases || hwdb.units[i].gen != token >> 12 ||
        hwdb.units[i].state != U_STARTING)
        return nullptr;
    return &hwdb.units[i];
}

/* The key the driver took the node by: its first the driver's table has. */
static const char *matched_key(const struct hw_node *n)
{
    const struct hw_match *m = hwdb.g.drivers[n->driver].match;

    for (unsigned k = 0; k < n->nkeys; k++)
        for (const struct hw_match *x = m; x->key; x++)
            if (hw_streq(x->key, n->keys[k]))
                return n->keys[k];
    return n->nkeys ? n->keys[0] : "";
}

static void bind_reply(struct unit *u, struct HwdbMsg *m)
{
    const struct hw_lease *ls = lease_of(u);

    m->hm_Flags = u->flags | (u->deaths ? DRVF_AGAIN : 0);
    m->hm_NRanges = ls->nranges;
    m->hm_NDevices = ls->nnodes;
    for (unsigned r = 0; r < ls->nranges; r++)
        m->hm_Range[r] = (struct DrvRange){
            .dr_Lease = u->lease[r], .dr_Base = ls->ranges[r].base, .dr_Size = ls->ranges[r].size,
        };
    for (unsigned i = 0; i < ls->nnodes; i++) {
        const struct hw_node *n = &hwdb.g.nodes[ls->nodes[i]];
        struct DrvDevice *d = &m->hm_Device[i];
        const char *key = matched_key(n);
        unsigned k = 0;

        *d = (struct DrvDevice){ .dd_Line = DRV_NOLINE, .dd_Unit = n->unit };
        for (unsigned q = 0; q < n->nres; q++) {
            const struct hw_res *r = &n->res[q];

            if ((r->kind == HW_MMIO || r->kind == HW_ECAM) && d->dd_Size == 0) {
                d->dd_Base = r->base;
                d->dd_Size = r->size;
                for (unsigned k = 0; k < r->ncells && k < 4; k++)
                    d->dd_Cells[k] = r->cells[k];
            }
            if (r->kind == HW_MSIX) {
                d->dd_MsixTable = r->base;
                d->dd_MsiDoorbell = r->ncells == 2 ? (uint64_t)r->cells[0] << 32 | r->cells[1] : 0;
            }
            if (r->kind == HW_IRQ && r->base != HW_NOLINE && d->dd_Line == DRV_NOLINE)
                for (unsigned l = 0; l < ls->nlines; l++)
                    if (ls->lines[l] == r->base)
                        d->dd_Line = l;
        }
        /* MSI-X's: its lines, in order from dd_Line (hw_lease_late's order). */
        if (d->dd_MsiDoorbell && d->dd_Line != DRV_NOLINE) {
            for (unsigned q = 0; q < n->nres; q++)
                d->dd_NLines += n->res[q].kind == HW_IRQ && n->res[q].base != HW_NOLINE;
        } else if (d->dd_Line != DRV_NOLINE) {
            /* Its own lines, as far as they follow each other in the
             * lease (they do for a device alone in one: the G720's). */
            unsigned l = d->dd_Line;

            for (unsigned q = 0; q < n->nres; q++) {
                const struct hw_res *r = &n->res[q];

                if (r->kind != HW_IRQ || r->base == HW_NOLINE)
                    continue;
                if (l >= ls->nlines || ls->lines[l] != r->base)
                    break;
                l++;
            }
            d->dd_NLines = l - d->dd_Line;
        }
        for (; key[k] && k < sizeof d->dd_Key - 1; k++)
            d->dd_Key[k] = key[k];
    }
}

static void started(struct unit *u, const struct HwdbMsg *m)
{
    const struct hw_lease *ls = lease_of(u);
    uint32_t accepted = m->hm_Accepted;     /* read once: it's the driver's memory */

    u->accepted = accepted;
    for (unsigned i = 0; i < ls->nnodes; i++) {
        for (unsigned k = 0; k < sizeof u->ports[i]; k++)
            u->ports[i][k] = m->hm_Ports[i][k];
        u->ports[i][sizeof u->ports[i] - 1] = '\0';
        if (accepted & 1u << i)
            hw_bound(&hwdb.g, ls->nodes[i]);
        else
            hw_unbind(&hwdb.g, ls->nodes[i], HW_S_DECLINED, nullptr);
    }
    u->state = U_RUNNING;
    if (accepted && hw_streq(unit_driver(u), "power"))
        power_up();
    if (accepted && hw_streq(unit_driver(u), "display"))
        display_up();
}

static void serve(unsigned c)
{
    struct HwdbMsg *m;

    if (conn_unit[c] == CONN_ACPID) {
        acpid_serve(&conns[c]);
        return;
    }
    if (conn_unit[c] == CONN_POWER) {
        power_serve(&conns[c]);
        return;
    }
    if (conn_unit[c] == CONN_DISPLAY) {
        display_serve(&conns[c]);
        return;
    }
    if (conn_unit[c] == CONN_INPUT) {
        input_conn_serve(&conns[c]);
        return;
    }
    while ((m = (struct HwdbMsg *)GetMsgSized(&conns[c], sizeof *m))) {
        uint32_t op = m->hm_Op, token = m->hm_Token;
        struct unit *u = unit_of(token);
        uint32_t idx = u ? (uint32_t)(u - hwdb.units) : UINT32_MAX;

        m->hm_Msg.mn_Error = 0;
        if (u && op == HWDB_BIND && conn_unit[c] == UINT32_MAX) {
            conn_unit[c] = idx;
            bind_reply(u, m);
        } else if (u && op == HWDB_STARTED && conn_unit[c] == idx) {
            started(u, m);
        } else if (u && op == HWDB_PCI_HOST && conn_unit[c] == idx &&
                   hw_streq(unit_driver(u), "pci")) {
            if (!pci_host(u, m))
                m->hm_Msg.mn_Error = (int32_t)ERR_BADARG;
        } else if (u && op == HWDB_PCI_FUNCTION && conn_unit[c] == idx &&
                   hw_streq(unit_driver(u), "pci")) {
            m->hm_Msg.mn_Error = (int32_t)pci_function(u, m);
        } else {
            m->hm_Msg.mn_Error = (int32_t)ERR_BADARG;
        }
        ReplyMsg(&conns[c], &m->hm_Msg);
    }
}

/* Drivers at "hwdb", acpid at "hwdb.acpi". */
static void accept_all(void)
{
    for (unsigned p = 0; p < 2; p++)
        for (;;) {
            struct MsgPort mp;

            if (!(p ? acpid.port : hwdb.port) ||
                AcceptPort(&mp, p ? acpid.port : hwdb.port, SIG_CONN) != 0)
                break;
            if (nconns == CONNS) {
                ClosePort(&mp);
                continue;
            }
            conn_unit[nconns] = p ? CONN_ACPID : UINT32_MAX;
            conns[nconns++] = mp;
        }
}

bool server_add_conn(const struct MsgPort *mp, uint32_t mark)
{
    if (nconns == CONNS)
        return false;
    conn_unit[nconns] = mark;
    conns[nconns++] = *mp;
    return true;
}

struct MsgPort *server_conn(uint32_t mark)
{
    for (unsigned c = 0; c < nconns; c++)
        if (conn_unit[c] == mark)
            return &conns[c];
    return nullptr;
}

/* Each connection's messages; one gone is closed, unless a unit's. */
static void serve_all(void)
{
    for (unsigned c = 0; c < nconns;) {
        serve(c);
        if (PortGone(&conns[c]) && (conn_unit[c] == UINT32_MAX || conn_unit[c] == CONN_ACPID ||
                                    conn_unit[c] == CONN_POWER || conn_unit[c] == CONN_DISPLAY ||
                                    conn_unit[c] == CONN_INPUT)) {
            if (conn_unit[c] == CONN_POWER)
                power_down();
            if (conn_unit[c] == CONN_DISPLAY)
                display_down();
            if (conn_unit[c] == CONN_INPUT)
                input_conn_down();
            ClosePort(&conns[c]);
            conns[c] = conns[--nconns];
            conn_unit[c] = conn_unit[nconns];
        } else {
            c++;
        }
    }
}

/* Before matching: acpid's namespace, until it's done, dead or late. */
static void settle_acpid(void)
{
    if (!hwdb.rsdp || !acpid_start(hwdb.rsdp))
        return;
    while (acpid_waiting()) {
        uint64_t got;

        ArmTimer(acpid.until, SIG_TIMER);
        got = WaitPorts(conns, nconns, 1ULL << SIG_PORT | 1ULL << SIG_CONN | 1ULL << SIG_DEAD |
                                           1ULL << SIG_ACPI_PORT | 1ULL << SIG_TIMER);
        if (got & (1ULL << SIG_PORT | 1ULL << SIG_ACPI_PORT))
            accept_all();
        serve_all();
        acpid_reap();
    }
    ArmTimer(0, 0);
    acpid_finish();
}

/* --- The report. --- */

/* The lines worth a boot's console: leases, what isn't bound, the count. */
static void report_line(void *, const char *line)
{
    bool want = hw_strneq(line, "hwdb: ", 6);

    for (const char *p = line; *p && !want; p++)
        want = hw_strneq(p, ", failed", 8) || hw_strneq(p, ", waits for", 11);
    if (want)
        say(line);
}

void hwdb_report(void)
{
    hw_report(&hwdb.g, false, report_line, nullptr);
}

/* What the drivers made of the description, once they've settled, for
 * comparing two descriptions of one machine (make compare-hw): each
 * device a process driver bound or failed, by its registers and line,
 * and how many each declined. Sorted by address, so that the order the
 * description lists them in doesn't show. */
static uint64_t first_reg(const struct hw_node *n)
{
    for (unsigned i = 0; i < n->nres; i++)
        if (n->res[i].kind == HW_MMIO || n->res[i].kind == HW_ECAM)
            return n->res[i].base;
    return UINT64_MAX;
}

void hwdb_facts(void)
{
    uint64_t last = 0;
    bool any = true;

    for (unsigned pass = 0; any; pass++) {
        uint64_t next = UINT64_MAX;
        hw_id at = HW_NONE;

        any = false;
        for (hw_id i = 1; i < hwdb.g.count; i++) {
            const struct hw_node *n = &hwdb.g.nodes[i];
            uint64_t b = first_reg(n);

            if (n->driver == HW_NODRV || hwdb.g.drivers[n->driver].kind != HW_DRV_PROCESS ||
                (n->state != HW_S_BOUND && n->state != HW_S_FAILED) || b == UINT64_MAX ||
                (pass && b <= last) || b >= next)
                continue;
            next = b;
            at = i;
        }
        if (at != HW_NONE) {
            const struct hw_node *n = &hwdb.g.nodes[at];
            struct hw_line l = { 0 };

            hw_put(&l, "hw: ");
            hw_put(&l, hwdb.g.drivers[n->driver].name);
            hw_put(&l, n->state == HW_S_BOUND ? " bound " : " failed ");
            hw_put_hex(&l, next);
            for (unsigned k = 0; k < n->nres; k++)
                if (n->res[k].kind == HW_IRQ && n->res[k].base != HW_NOLINE) {
                    hw_put(&l, " line ");
                    hw_put_dec(&l, n->res[k].base);
                    break;
                }
            if (n->state == HW_S_FAILED && n->why) {
                hw_put(&l, ": ");
                hw_put(&l, n->why);
            }
            say(l.text);
            last = next;
            any = true;
        }
    }
    for (unsigned d = 0; d < hwdb.g.ndrivers; d++) {
        unsigned declined = 0;
        struct hw_line l = { 0 };

        for (hw_id i = 1; i < hwdb.g.count; i++)
            declined += hwdb.g.nodes[i].driver == d && hwdb.g.nodes[i].state == HW_S_DECLINED;
        if (!declined)
            continue;
        hw_put(&l, "hw: ");
        hw_put(&l, hwdb.g.drivers[d].name);
        hw_put(&l, " declined ");
        hw_put_dec(&l, declined);
        say(l.text);
    }
}

/* --- The system (not the test): dos, then the Shell, kept going. --- */

static Handle sys_dos, sys_shell;
static uint64_t sys_due;

static void system_step(void)
{
    struct ProcessInfo pi;
    struct dos d;
    int64_t p;

    if (hwdb.stopping) {
        bool any = false;

        for (uint32_t i = 0; i < hwdb.g.nleases; i++)
            any |= hwdb.units[i].proc != 0;
        hwdb.finished = !any;
        return;
    }
    if (hwdb.test || hwdb_busy() || hwdb.now < sys_due)
        return;
    if (sys_dos && QueryProcess(sys_dos, &pi) == 0 && pi.pi_State >= PROCS_EXITED) {
        say("hwdb: dos ended: restarting it");
        CloseHandle(sys_dos);
        sys_dos = 0;
    }
    if (!sys_dos) {
        if ((p = CreateProcess("dos", 0, SIG_DEAD, 0)) < 0) {
            say("hwdb: dos couldn't be started");
            sys_due = hwdb.now + 1000 * MS;
            return;
        }
        sys_dos = (Handle)p;
    }
    if (sys_shell && QueryProcess(sys_shell, &pi) == 0 && pi.pi_State >= PROCS_EXITED) {
        CloseHandle(sys_shell);
        sys_shell = 0;
        if (hwdb.end) {             /* EndShell, on a board that goes back to its other system */
            say("hwdb: the Shell ended: stopping");
            CloseHandle(sys_dos);
            sys_dos = 0;
            hwdb.stopping = true;
            for (uint32_t i = 0; i < hwdb.g.nleases; i++)
                unit_kill(&hwdb.units[i]);
            return;
        }
        sys_due = hwdb.now + 100 * MS;  /* a new Shell after one ends */
        return;
    }
    if (sys_shell)
        return;
    /* The Shell from C:, with the console's input, once dos answers. */
    if (dos_open(&d, SIG_DOS_CLIENT) != 0) {
        sys_due = hwdb.now + 50 * MS;
        return;
    }
    p = dos_run(&d, "C:shell", "", SIG_DEAD, PROCF_HELD);
    dos_close(&d);
    if (p < 0) {
        say("hwdb: no C:shell to start");
        sys_due = hwdb.now + 5000 * MS;
        return;
    }
    if (GrantConsole(hwdb.root, (Handle)p) != 0 || StartProcess((Handle)p) != 0) {
        CloseHandle((Handle)p);
        sys_due = hwdb.now + 1000 * MS;
        return;
    }
    sys_shell = (Handle)p;
}

/* --- The loop. --- */

static uint64_t next_deadline(void)
{
    uint64_t next = hwdb.test ? test_deadline() : (!sys_shell || sys_due > hwdb.now ? hwdb.now + 50 * MS : 0);

    for (uint32_t i = 0; i < hwdb.g.nleases; i++) {
        const struct unit *u = &hwdb.units[i];

        if ((u->state == U_DUE || u->state == U_STARTING) && (!next || u->due < next))
            next = u->due;
    }
    return next;
}

static void timers(void)
{
    for (uint32_t i = 0; i < hwdb.g.nleases; i++) {
        struct unit *u = &hwdb.units[i];

        if (u->state == U_DUE && hwdb.now >= u->due) {
            start(u);
        } else if (u->state == U_STARTING && hwdb.now >= u->due && !u->late) {
            u->late = true;
            KillProcess(u->proc);
        }
    }
}

int main(uint64_t arg, uint64_t root)
{
    int64_t port;

    hwdb.root = (Handle)root;
    hwdb.test = arg & HWDB_TEST;
    hwdb.end = arg & HWDB_END;
    hwdb.now = CurrentTime();
    if (!build()) {
        say("hwdb: the firmware's description is unusable");
        return 1;
    }
    if ((port = CreatePort("hwdb", SIG_PORT)) < 0) {
        say("hwdb: no port");
        return 3;
    }
    hwdb.port = (Handle)port;
    settle_acpid();
    smmus();
    if (hwdb.test)
        power_model();
    hw_prepare(&hwdb.g);
    hwdb.units = hw_alloc(&hwdb.arena, (hwdb.g.maxleases ? hwdb.g.maxleases : 1) * sizeof *hwdb.units);
    if (hwdb.g.full || !hwdb.units) {
        say("hwdb: out of memory for the graph");
        return 2;
    }

    while (!hwdb.finished) {
        uint64_t next, got;

        pump();
        input_keep();
        if (hwdb.test)
            test_step();
        else
            system_step();
        if (hwdb.finished)
            break;
        next = next_deadline();
        if (input_due() && (!next || input_due() < next))
            next = input_due();
        ArmTimer(next, next ? SIG_TIMER : 0);
        got = WaitPorts(conns, nconns, 1ULL << SIG_PORT | 1ULL << SIG_CONN | 1ULL << SIG_DEAD |
                                           1ULL << SIG_ACPI_PORT | 1ULL << SIG_TIMER);
        hwdb.now = CurrentTime();
        if (got & (1ULL << SIG_PORT | 1ULL << SIG_ACPI_PORT))
            accept_all();
        serve_all();
        reap();
        acpid_reap();
        timers();
    }
    acpid_stop();
    for (uint32_t i = 0; i < hwdb.g.nleases; i++)
        for (unsigned r = 0; r < HW_LEASE_RANGES; r++)
            if (hwdb.units[i].lease[r])
                CloseHandle(hwdb.units[i].lease[r]);
    DeletePort(hwdb.port);
    return hwdb.errors ? 10 : 0;
}
