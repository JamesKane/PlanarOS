/*
 * drv/gpu-panthor/main.c -- the CIX's Mali-G720, its CSF, and a queue
 * per client (v5-plan step 5, §5.1)
 *
 * hwdb gives it the GPU's two windows, its RCSU's and its registers',
 * and RST0's page, and its three lines: job, MMU and GPU, in that order
 * (ACPI's, as the CIX patches to panthor read them). Nothing is read
 * from the GPU until it has power, so Bind takes the device and the
 * rest is done from the loop:
 *
 * 1. power's set "gpu": SCMI domain 21 and the GPU's two clocks. hwdb
 *    votes for it before the lease (a quirk), so it stays on until a
 *    dead driver's GPU has been stopped.
 * 2. GPUP's power resource (its _ON): bits 2-12 of the RCSU's power
 *    control, then the memory repair, if RST0's strap says it's
 *    wanted (DMRP). Then a reset pulse, RST0's GPU bit, as the vendor's
 *    tree has it (under ACPI UEFI leaves the GPU held in reset), and
 *    the Q-channel's clock gating (bit 0), as the patches have it.
 * 3. A soft reset, the GPU's own, and its features. Cores the RCSU
 *    says are harvested are left out.
 * 4. The firmware, once dos can give it (fw.c); then it's pinged
 *    every 12 s.
 *
 * Clients connect to "gpu.0". Each has an address space of its own,
 * with the Areas it lends at their own addresses, and at most one
 * queue (queue.c). What a client had goes with its connection.
 */
#include <stddef.h>

#include <planar/devices/gpu.h>
#include <planar/lease.h>
#include <planar/syscalls.h>

#include "panthor.h"

constexpr int64_t SIG_NAP = 24;
constexpr uint64_t RCSU = 0x15000000, REGS = 0x15010000, RST = 0x16000000;
constexpr uint64_t RETRY = 500 * MS;
constexpr uint64_t PING = 12000 * MS;

enum : uint32_t {               /* the RCSU's */
    RCSU_REPAIR_PASS = 0x10, RCSU_REPAIR_ENABLE = 0x14, RCSU_REPAIR_BUSY = 0x18,
    RCSU_POWER = 0x218, RCSU_HARVEST = 0x304,
};
enum : uint32_t {               /* RST0's (the vendor's reset-sky1.c): a bit clear holds it in reset */
    RST_S0_DOMAIN_0 = 0x400,    /* bit 6: the GPU's */
    RST_REPAIR_VALID = 0x504,   /* bit 4: the GPU's memory wants repair */
};

struct gpu gpu;

static struct drv *the;
static struct drv_dev *dev0;
static bool powered, reset_done;

void nap(uint64_t ms)
{
    ArmTimer(CurrentTime() + ms * MS, SIG_NAP);
    Wait(1ULL << SIG_NAP);
}

void glog(const char *text)
{
    drv_log(the, dev0, text);
}

void say(const char *a, uint64_t v, unsigned base, const char *b)
{
    char t[120], dg[24];
    unsigned n = 0, k = 0;

    while (*a && n < 80)
        t[n++] = *a++;
    if (base == 16) {
        t[n++] = '0';
        t[n++] = 'x';
    }
    do
        dg[k++] = "0123456789abcdef"[v % base];
    while ((v /= base) && k < sizeof dg);
    while (k)
        t[n++] = dg[--k];
    while (*b && n < sizeof t - 1)
        t[n++] = *b++;
    t[n] = '\0';
    glog(t);
}

static bool leased(struct drv *d, uint64_t at, uint64_t size)
{
    for (unsigned k = 0; k < d->info.hm_NRanges; k++) {
        const struct DrvRange *rg = &d->info.hm_Range[k];

        if (at >= rg->dr_Base && at + size <= rg->dr_Base + rg->dr_Size)
            return true;
    }
    return false;
}

static uint32_t rr(uint32_t off)
{
    return *(volatile uint32_t *)(gpu.rcsu + off);
}

static void rw(uint32_t off, uint32_t v)
{
    *(volatile uint32_t *)(gpu.rcsu + off) = v;
}

/* --- Bring-up. --- */

/* GPUP.PPRS._ON, then the Q-channel. */
static bool power_on(void)
{
    uint32_t valid;

    rw(RCSU_POWER, rr(RCSU_POWER) | 0x1000 | 0x0ffc);
    nap(1);
    valid = *(volatile uint32_t *)(gpu.rst + RST_REPAIR_VALID);
    if (valid & 1u << 4) {
        uint64_t until = CurrentTime() + 100 * MS;

        while (rr(RCSU_REPAIR_BUSY) >> 16)
            if (CurrentTime() > until) {
                glog("the GPU's memory repair stayed busy");
                return false;
            }
        rw(RCSU_REPAIR_ENABLE, 1);
        while ((rr(RCSU_REPAIR_PASS) >> 1 & 3) != 3)
            if (CurrentTime() > until) {
                glog("the GPU's memory repair didn't pass");
                rw(RCSU_REPAIR_ENABLE, 0);
                return false;
            }
        rw(RCSU_REPAIR_ENABLE, 0);
    }
    /* Its resets pulsed, every start: the domain may have been off since
     * (the last driver's vote went with it), and the GPU comes up in an
     * unknown state without one (the vendor's order: power, clocks,
     * reset asserted and released, then the Q-channel). Read-modify-
     * write: the word is the DDR controller's, the GIC's and the NoCs'
     * too. The vendor holds it 10-20 ms (reset-sky1.c). Not the RCSU's
     * reset (RST_RCSU bit 28): that takes away the non-secure access TF-A
     * gave the RCSU, and the next access to it is refused (IDM). */
    *(volatile uint32_t *)(gpu.rst + RST_S0_DOMAIN_0) &= ~(1u << 6);
    nap(20);
    *(volatile uint32_t *)(gpu.rst + RST_S0_DOMAIN_0) |= 1u << 6;
    nap(20);
    rw(RCSU_POWER, rr(RCSU_POWER) | 1);
    return true;
}

static bool soft_reset(void)
{
    gw(GPU_INT_MASK, 0);
    gw(JOB_INT_MASK, 0);
    gw(MMU_INT_MASK, 0);
    gw(MCU_CONTROL, MCU_DISABLE);
    gw(GPU_INT_CLEAR, ~0u);
    gw(GPU_CMD, GPU_SOFT_RESET);
    for (uint64_t until = CurrentTime() + 100 * MS; !(gr(GPU_INT_RAWSTAT) & GPU_IRQ_RESET_COMPLETED);) {
        if (CurrentTime() > until) {
            glog("the GPU didn't come out of its reset");
            return false;
        }
        nap(1);
    }
    gw(GPU_INT_CLEAR, ~0u);
    gw(JOB_INT_CLEAR, ~0u);
    gw(MMU_INT_CLEAR, ~0u);
    return true;
}

static bool features(void)
{
    uint32_t harvest = rr(RCSU_HARVEST);

    gpu.id = gr(GPU_ID);
    gpu.csf_id = gr(GPU_CSF_ID);
    gpu.mmu_features = gr(GPU_MMU_FEATURES);
    gpu.as_present = gr(GPU_AS_PRESENT);
    gpu.va_bits = gpu.mmu_features & 0xff;
    gpu.cores = (gr(GPU_SHADER_PRESENT) | (uint64_t)gr(GPU_SHADER_PRESENT + 4) << 32) &
                (~(uint64_t)((harvest & 0xffffff0) >> 4) & 0x550555);
    say("GPU_ID ", gpu.id, 16, "");
    say("  shader cores ", gpu.cores, 16, "");
    if (gpu.id >> 28 != 12 || gpu.va_bits < 40 || gpu.va_bits > 48 || !gpu.cores ||
        (gpu.as_present & 3) != 3) {
        glog("not the G720 this driver knows");
        return false;
    }
    gw(GPU_INT_MASK, GPU_IRQ_FAULT);
    return true;
}

static void bring_up(struct drv *d)
{
    int r;

    if (gpu.up || gpu.broken || CurrentTime() < gpu.next_try)
        return;
    gpu.next_try = CurrentTime() + RETRY;
    if (!powered) {
        if (!power_on() || !soft_reset() || !features() || !mmu_init(d)) {
            gpu.broken = true;
            return;
        }
        powered = reset_done = true;
    }
    if ((r = fw_boot(d)) == ERR_NOTFOUND)
        return;                         /* dos, or its file, isn't there yet */
    if (r != 0) {
        glog("the firmware didn't start: given up");
        gpu.broken = true;
        return;
    }
    gpu.next_ping = CurrentTime() + PING;
    {
        char t[96] = "CSF up: interface ";
        unsigned n = 18;
        uint32_t v = gpu.glb->version, parts[3] = { v >> 24, v >> 16 & 0xff, gpu.nslots };

        for (unsigned i = 0; i < 3; i++) {
            char dg[12];
            unsigned k = 0;

            do
                dg[k++] = (char)('0' + parts[i] % 10);
            while ((parts[i] /= 10) && k < sizeof dg);
            while (k)
                t[n++] = dg[--k];
            for (const char *x = i == 0 ? "." : i == 1 ? ", queues for " : " clients"; *x; x++)
                t[n++] = *x;
        }
        t[n] = '\0';
        glog(t);
    }
}

/* --- Clients. --- */

static struct client *client_of(unsigned c)
{
    return c < GPU_CLIENTS ? &gpu.clients[c] : nullptr;
}

static void unmap_all(struct client *c)
{
    for (unsigned i = 0; i < GPU_MAPS; i++)
        if (c->maps[i].size) {
            vm_unmap(&c->vm, c->maps[i].addr, c->maps[i].size);
            ReleaseArea((void *)c->maps[i].addr);
            c->maps[i] = (struct map){ 0 };
        }
}

static int64_t map(struct client *c, uint64_t addr, uint64_t size)
{
    struct AreaInfo ai;
    struct map *m = nullptr;

    if (QueryArea((void *)addr, &ai) != 0)
        return ERR_NOTFOUND;            /* not shared with the server */
    if ((uint64_t)ai.ai_Address != addr || ai.ai_Size != size || ai.ai_Area)
        return ERR_BADARG;              /* a whole Area, and not the server's own */
    if (!(ai.ai_Flags & AREAF_NC))
        return ERR_RIGHTS;              /* the GPU doesn't see the CPU's caches */
    for (unsigned i = 0; i < GPU_MAPS; i++) {
        if (c->maps[i].size && c->maps[i].addr == addr)
            return ERR_EXISTS;
        if (!c->maps[i].size && !m)
            m = &c->maps[i];
    }
    if (!m)
        return ERR_FULL;
    if (!vm_map(&c->vm, addr, addr, size, ai.ai_Rights & AREAR_WRITE, false))
        return ERR_NOMEM;
    *m = (struct map){ .addr = addr, .size = size };
    return 0;
}

static int64_t unmap(struct client *c, uint64_t addr)
{
    for (unsigned i = 0; i < GPU_MAPS; i++)
        if (c->maps[i].size && c->maps[i].addr == addr) {
            if (c->slot >= 0)
                return ERR_BUSY;        /* its queue may be using it */
            vm_unmap(&c->vm, addr, c->maps[i].size);
            ReleaseArea((void *)addr);
            c->maps[i] = (struct map){ 0 };
            return 0;
        }
    return ERR_NOTFOUND;
}

static bool mapped(const struct client *c, uint64_t addr, uint64_t size)
{
    for (unsigned i = 0; i < GPU_MAPS; i++)
        if (c->maps[i].size && addr >= c->maps[i].addr &&
            addr + size <= c->maps[i].addr + c->maps[i].size)
            return true;
    return false;
}

/* A held GPU_EVENT answered, if a fault has come since the last. */
static void tell(struct drv *d, struct client *c)
{
    struct GpuMsg *m = (struct GpuMsg *)c->event;

    if (!m || c->faults == c->told)
        return;
    m->gm_Fault = c->fault;
    m->gm_FaultAddress = c->fault_addr;
    m->gm_Faults = c->faults;
    m->gm_Msg.mn_Error = 0;
    c->told = c->faults;
    c->event = nullptr;
    drv_reply(d, c->event_ci, &m->gm_Msg);
}

static void message(struct drv *d, struct drv_dev *, unsigned ci, struct Message *msg)
{
    struct GpuMsg *m = (struct GpuMsg *)msg;
    struct client *c = client_of(ci);
    int64_t e = 0;

    if (msg->mn_Length < sizeof *m || !c) {
        msg->mn_Error = (int32_t)ERR_BADARG;
        drv_reply(d, ci, msg);
        return;
    }
    if (!gpu.up) {
        m->gm_Msg.mn_Error = (int32_t)(gpu.broken ? ERR_NOSYS : ERR_BUSY);
        drv_reply(d, ci, msg);
        return;
    }
    if (m->gm_Op != GPU_OPEN && !c->open) {
        m->gm_Msg.mn_Error = (int32_t)ERR_BADARG;
        drv_reply(d, ci, msg);
        return;
    }
    switch (m->gm_Op) {
    case GPU_OPEN:
        if (c->open) {
            e = ERR_EXISTS;
            break;
        }
        *c = (struct client){ .slot = -1, .task = m->gm_Task };
        if (!vm_new(&c->vm)) {
            e = ERR_NOMEM;
            break;
        }
        c->open = true;
        m->gm_Server = FindTask(nullptr);
        m->gm_Kind = GPU_CSF;
        m->gm_Id = gpu.id;
        m->gm_Cores = gpu.cores;
        m->gm_Slots = gpu.nslots;
        m->gm_Regs = gpu.cs_regs;
        m->gm_Free = 0;
        for (unsigned s = 0; s < gpu.nslots; s++)
            m->gm_Free += !gpu.slots[s].owner;
        break;
    case GPU_MAP:
        e = map(c, m->gm_Address, m->gm_Size);
        m->gm_Device = m->gm_Address;   /* its own address */
        break;
    case GPU_UNMAP:
        e = unmap(c, m->gm_Address);
        break;
    case GPU_QUEUE:
        if (m->gm_RingSize < PAGE || m->gm_RingSize > (1u << 20) ||
            m->gm_RingSize & (m->gm_RingSize - 1) || m->gm_Ring & (m->gm_RingSize - 1) ||
            !mapped(c, m->gm_Ring, m->gm_RingSize) ||
            (m->gm_Sync && !mapped(c, m->gm_Sync, sizeof(struct GpuSync)))) {
            e = ERR_BADARG;
            break;
        }
        c->sync = m->gm_Sync;
        e = queue_start(d, c, m->gm_Ring, m->gm_RingSize, &m->gm_Input, &m->gm_Output,
                        &m->gm_Doorbell);
        break;
    case GPU_STATUS:
        m->gm_Fault = c->fault;
        m->gm_FaultAddress = c->fault_addr;
        m->gm_Faults = c->faults;
        break;
    case GPU_WAIT:
        queue_arm(c, m->gm_Seqno);
        break;
    case GPU_EVENT:
        if (c->event) {
            e = ERR_EXISTS;
            break;
        }
        c->event = msg;                 /* answered by tell(), at a fault */
        c->event_ci = ci;
        tell(d, c);
        return;
    default:
        e = ERR_BADARG;
    }
    m->gm_Msg.mn_Error = (int32_t)e;
    drv_reply(d, ci, msg);
}

static void gone(struct drv *d, struct drv_dev *, unsigned ci)
{
    struct client *c = client_of(ci);

    if (!c || !c->open)
        return;
    if (c->event) {                     /* answered, to nobody: the library's count */
        c->event->mn_Error = (int32_t)ERR_PORT_DIED;
        drv_reply(d, c->event_ci, c->event);
    }
    queue_stop(d, c);                   /* first: nothing runs on its memory */
    unmap_all(c);
    vm_free(&c->vm);
    *c = (struct client){ .slot = -1 };
}

/* --- The loop. --- */

static void interrupt(struct drv *, struct drv_dev *)
{
    uint32_t st;

    if (!reset_done)
        return;
    queue_events();
    mmu_interrupt();
    if ((st = gr(GPU_INT_STAT))) {
        gw(GPU_INT_CLEAR, st);
        if (st & GPU_IRQ_FAULT) {
            say("a GPU fault: status ", gr(GPU_FAULT_STATUS), 16, "");
            say("  at ", gr(GPU_FAULT_ADDR) | (uint64_t)gr(GPU_FAULT_ADDR + 4) << 32, 16, "");
        }
    }
}

static bool poll(struct drv *d)
{
    bring_up(d);
    if (gpu.up && !gpu.broken && CurrentTime() >= gpu.next_ping) {
        fw_ping();
        gpu.next_ping = CurrentTime() + PING;
    }
    /* A queue that faulted is stopped: its group may be stuck. Its
     * client hears of it, if it's listening. */
    for (unsigned i = 0; i < GPU_CLIENTS; i++) {
        struct client *c = &gpu.clients[i];

        if (c->open)
            tell(d, c);
        if (c->open && c->fault && c->slot >= 0)
            queue_stop(d, c);
    }
    return false;
}

static uint64_t deadline(struct drv *)
{
    if (gpu.broken)
        return 0;
    return gpu.up ? gpu.next_ping : gpu.next_try;
}

static int bind(struct drv *d, struct drv_dev *dev)
{
    the = d;
    dev0 = dev;
    if (dev->info->dd_Base != RCSU || !leased(d, REGS, 0x480000) || !leased(d, RST, PAGE)) {
        glog("not the CIX's G720: its windows aren't leased");
        return DRV_DECLINE;
    }
    if (dev->info->dd_NLines < 3) {
        glog("its three lines aren't there");
        return DRV_DECLINE;
    }
    gpu.rcsu = (volatile uint8_t *)RCSU;
    gpu.regs = (volatile uint8_t *)REGS;
    gpu.rst = (volatile uint8_t *)RST;
    gpu.next_try = CurrentTime();
    for (unsigned i = 0; i < GPU_CLIENTS; i++)
        gpu.clients[i].slot = -1;
    return DRV_OK;
}

static int start(struct drv *d, struct drv_dev *dev)
{
    int64_t e = drv_publish(d, dev, "gpu");

    return e < 0 ? (int)e : 0;
}

static const struct drv_ops ops = {
    .name = "gpu-panthor", .bind = bind, .start = start, .interrupt = interrupt,
    .message = message, .message_size = sizeof(struct GpuMsg), .poll = poll,
    .deadline = deadline, .gone = gone, .every_line = true,
};

int main(uint64_t token)
{
    drv_main(token, &ops);
}
