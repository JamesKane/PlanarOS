/*
 * servers/hwdb/acpid.c -- acpid, as hwdb runs it (docs §9.4, v4-plan step 4)
 *
 * When the firmware described the machine by ACPI, hwdb starts acpid
 * before matching anything: held, the firmware's tables granted to it,
 * then started, with a token as its argument. Its connection to the port
 * "hwdb.acpi" is one of hwdb's (server.c), marked as acpid's. hwdb adds
 * the devices it sends to the graph, and matches once it's done, or dead,
 * or late (ACPID_MS): then the static tables alone describe the machine.
 *
 * Each window acpid asks for is checked (R5). The firmware's own tables
 * it already maps. Refused: anything over the interrupt controller or
 * an IOMMU, a device the kernel drives, or one a driver's lease holds.
 * The rest is leased to acpid, and the kernel refuses memory and its own
 * devices again. Each refusal is said once.
 *
 * acpid isn't restarted if it dies: its _INI methods ran once, and
 * nothing asks it for more yet. Its leases are taken back when it dies,
 * or when hwdb ends.
 */
#include <planar/acpid.h>
#include <planar/syscalls.h>

#include "server.h"

constexpr uint64_t ACPID_MS = 10000;        /* to load the namespace and describe it */
constexpr uint64_t PAGE = 4096;
constexpr uint64_t WINDOW_MAX = 1 << 20;    /* the most one region asks at once */

struct acpid acpid;

static struct AcpidMsg copy;        /* what came: acpid's, in shared memory */

/* --- Windows. --- */

static bool in_tables(uint64_t base, uint64_t size)
{
    for (uint64_t p = base; p < base + size; p += PAGE) {
        bool covered = false;

        for (unsigned i = 0; i < acpid.ntables && !covered; i++) {
            uint64_t lo = acpid.tables[i].base & ~(PAGE - 1);
            uint64_t hi = acpid.tables[i].base + acpid.tables[i].size;

            covered = p >= lo && p < hi;
        }
        if (!covered)
            return false;
    }
    return true;
}

static bool overlaps(const struct hw_node *n, uint64_t base, uint64_t size)
{
    for (unsigned i = 0; i < n->nres; i++) {
        const struct hw_res *r = &n->res[i];

        if ((r->kind == HW_MMIO || r->kind == HW_ECAM) && r->base < base + size &&
            base < r->base + r->size)
            return true;
    }
    return false;
}

/* Why hwdb won't lease the window to acpid, or nullptr. */
static const char *refused(uint64_t base, uint64_t size, hw_id *who)
{
    for (hw_id i = 1; i < hwdb.g.count; i++) {
        const struct hw_node *n = &hwdb.g.nodes[i];

        if (!overlaps(n, base, size))
            continue;
        *who = i;
        if (hw_has_key(n, "arm,gic-v3") || hw_has_key(n, "arm,cortex-a15-gic") ||
            hw_has_key(n, "arm,gic-v3-its"))
            return "the interrupt controller's";
        if (hw_has_key(n, "arm,smmu-v3"))
            return "an IOMMU's";
        if (n->state == HW_S_KERNEL)
            return "a device the kernel drives";
    }
    for (uint32_t l = 0; l < hwdb.g.nleases; l++) {
        const struct hw_lease *ls = &hwdb.g.leases[l];

        for (unsigned r = 0; r < ls->nranges; r++)
            if (ls->ranges[r].base < base + size && base < ls->ranges[r].base + ls->ranges[r].size) {
                *who = ls->nodes[0];
                return "a driver's";
            }
    }
    return nullptr;
}

static void say_window(const char *what, uint64_t base, uint64_t size, hw_id who)
{
    struct hw_line l = { 0 };

    hw_put(&l, "hwdb: acpid's AML asked for ");
    hw_put_hex(&l, base);
    hw_put(&l, "+");
    hw_put_hex(&l, size);
    hw_put(&l, ": ");
    hw_put(&l, what);
    if (who != HW_NONE) {
        hw_put(&l, " (");
        hw_put_path(&l, &hwdb.g, who);
        hw_put(&l, ")");
    }
    say(l.text);
}

static int32_t window(struct AcpidMsg *m)
{
    uint64_t base = copy.am_Base, size = copy.am_Size;
    bool quiet = copy.am_Count == 1;        /* the probe's: it says what came of it */
    hw_id who = HW_NONE;
    const char *why;
    int64_t l;

    if ((base | size) & (PAGE - 1) || size == 0 || size > WINDOW_MAX || base + size < base)
        return (int32_t)ERR_BADARG;
    if (in_tables(base, size)) {
        m->am_Count = ACPID_TABLES;
        return 0;
    }
    m->am_Count = ACPID_REFUSED;
    if (!quiet)
        acpid.asked++;
    if ((why = refused(base, size, &who))) {
        if (!quiet) {
            say_window(why, base, size, who);
            acpid.refused++;
        }
        return (int32_t)ERR_RIGHTS;
    }
    if (acpid.nregions == ACPID_REGIONS) {
        if (!quiet)
            say_window("refused: hwdb has no room for more of acpid's windows", base, size,
                       HW_NONE);
        acpid.refused += !quiet;
        return (int32_t)ERR_NOMEM;
    }
    if ((l = CreateLease(hwdb.root, base, size, nullptr, 0)) < 0) {
        if (!quiet)
            say_window(l == ERR_RIGHTS ? "refused by the kernel (memory, or its own)"
                       : l == ERR_EXISTS ? "refused by the kernel (another lease holds it)"
                                         : "refused by the kernel",
                       base, size, HW_NONE);
        acpid.refused += !quiet;
        return (int32_t)l;
    }
    if (GrantLease((Handle)l, acpid.proc) != 0) {
        CloseHandle((Handle)l);
        return (int32_t)ERR_NOTFOUND;
    }
    acpid.regions[acpid.nregions++] = (struct acpid_region){ base, size, (Handle)l };
    if (!quiet)
        say_window("leased", base, size, HW_NONE);
    m->am_Count = ACPID_LEASED;
    m->am_Lease = (Handle)l;
    return 0;
}

/* After the probe: no window acpid holds may be a driver's, the
 * kernel's, the interrupt controller's or an IOMMU's. Checked against
 * the graph afresh, not by refused(), which made the choices. */
static void check_windows(void)
{
    for (unsigned w = 0; w < acpid.nregions; w++) {
        uint64_t base = acpid.regions[w].base, size = acpid.regions[w].size;
        bool bad = false;

        for (uint32_t l = 0; l < hwdb.g.nleases; l++)
            for (unsigned r = 0; r < hwdb.g.leases[l].nranges; r++)
                bad |= hwdb.g.leases[l].ranges[r].base < base + size &&
                       base < hwdb.g.leases[l].ranges[r].base + hwdb.g.leases[l].ranges[r].size;
        for (hw_id i = 1; i < hwdb.g.count; i++) {
            const struct hw_node *n = &hwdb.g.nodes[i];

            if (n->state == HW_S_KERNEL || hw_has_key(n, "arm,gic-v3") ||
                hw_has_key(n, "arm,gic-v3-its") || hw_has_key(n, "arm,smmu-v3"))
                bad |= overlaps(n, base, size);
        }
        if (bad) {
            struct hw_line l = { 0 };

            hw_put(&l, "hwdb: acpid holds ");
            hw_put_hex(&l, base);
            hw_put(&l, "+");
            hw_put_hex(&l, size);
            hw_put(&l, ", which it mustn't: a driver's, the kernel's, or the GIC's or an IOMMU's");
            say(l.text);
            hwdb.errors++;
        }
    }
}

/* --- The protocol. --- */

void acpid_serve(struct MsgPort *mp)
{
    struct AcpidMsg *m;

    while ((m = (struct AcpidMsg *)GetMsgSized(mp, sizeof *m))) {
        int32_t err = 0;

        memcpy(&copy, m, sizeof copy);
        if (!acpid.proc || copy.am_Token != acpid.token) {
            err = (int32_t)ERR_BADARG;
        } else if (copy.am_Op == ACPID_HELLO) {
            m->am_Base = acpid.rsdp;
        } else if (copy.am_Op == ACPID_DEVICE && !acpid.done) {
            hw_ns_add(&hwdb.g, &copy.am_Device);
            acpid.devices++;
        } else if (copy.am_Op == ACPID_DONE && !acpid.done) {
            acpid.done = true;
            if (copy.am_Count != acpid.devices)
                hwdb.errors++;
        } else if (copy.am_Op == ACPID_REGION) {
            err = window(m);
        } else if (copy.am_Op == ACPID_PROBED && acpid.done && !acpid.probed) {
            acpid.probed = true;
            if (copy.am_Count != 1)
                hwdb.errors++;
            check_windows();
        } else {
            err = (int32_t)ERR_BADARG;
        }
        m->am_Msg.mn_Error = err;
        ReplyMsg(mp, &m->am_Msg);
    }
}

/* --- Its life. --- */

bool acpid_start(uint64_t rsdp)
{
    int64_t p, port;

    acpid.rsdp = rsdp;
    acpid.token = (uint32_t)(CurrentTime() >> 4) | 1;
    if ((port = CreatePort("hwdb.acpi", SIG_ACPI_PORT)) < 0)
        return false;
    acpid.port = (Handle)port;
    if ((p = CreateProcess("acpid", acpid.token | (hwdb.test ? ACPID_ARG_PROBE : 0), SIG_DEAD,
                           PROCF_HELD)) < 0) {
        say(p == ERR_NOTFOUND ? "hwdb: acpid isn't built: the static tables alone"
                              : "hwdb: acpid couldn't be made: the static tables alone");
        hwdb.errors += hwdb.test;
        return false;
    }
    if (GrantFirmware(hwdb.root, (Handle)p) != 0 || StartProcess((Handle)p) != 0) {
        CloseHandle((Handle)p);
        say("hwdb: acpid couldn't be started: the static tables alone");
        hwdb.errors += hwdb.test;
        return false;
    }
    acpid.proc = (Handle)p;
    acpid.until = CurrentTime() + ACPID_MS * MS;
    return true;
}

/* Take back what acpid held, once it's dead. */
static void reclaim(void)
{
    for (unsigned i = 0; i < acpid.nregions; i++) {
        RevokeLease(acpid.regions[i].lease);
        CloseHandle(acpid.regions[i].lease);
    }
    acpid.nregions = 0;
    CloseHandle(acpid.proc);
    acpid.proc = 0;
}

bool acpid_reap(void)
{
    struct ProcessInfo pi;
    struct hw_line l = { 0 };

    if (!acpid.proc || QueryProcess(acpid.proc, &pi) != 0 || pi.pi_State < PROCS_EXITED)
        return false;
    if (!acpid.stopping) {
        hw_put(&l, "hwdb: acpid ");
        if (pi.pi_State == PROCS_FAULTED) {
            hw_put(&l, "faulted at ");
            hw_put_hex(&l, pi.pi_FaultPC);
        } else {
            hw_put(&l, "exited ");
            hw_put_dec(&l, (uint64_t)pi.pi_ExitCode);
        }
        hw_put(&l, acpid.done ? " after describing the namespace" : ": the static tables alone");
        say(l.text);
        hwdb.errors += hwdb.test;
    }
    reclaim();
    return true;
}

bool acpid_waiting(void)
{
    return acpid.proc && !acpid.done && CurrentTime() < acpid.until;
}

void acpid_stop(void)
{
    if (acpid.proc) {
        acpid.stopping = true;
        KillProcess(acpid.proc);
        while (!acpid_reap())
            Wait(1ULL << SIG_DEAD);
    }
    if (acpid.port)
        DeletePort(acpid.port);
    acpid.port = 0;
}

void acpid_finish(void)
{
    struct hw_line l = { 0 };

    if (!acpid.rsdp)
        return;
    if (!acpid.done && acpid.proc) {
        say("hwdb: acpid didn't finish in time: the static tables alone");
        hwdb.errors += hwdb.test;
    }
    hw_ns_done(&hwdb.g);
    hw_put(&l, "hwdb: acpid described ");
    hw_put_dec(&l, acpid.devices);
    hw_put(&l, " devices; its AML asked for ");
    hw_put_dec(&l, acpid.asked);
    hw_put(&l, " windows, ");
    hw_put_dec(&l, acpid.refused);
    hw_put(&l, " refused");
    say(l.text);
}
