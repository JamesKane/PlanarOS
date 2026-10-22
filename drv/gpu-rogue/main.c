/*
 * drv/gpu-rogue/main.c -- the RV2's BXE-2-32, its MIPS firmware, and a
 * server queue per client (v5-plan step 6, §5.1)
 *
 * hwdb gives it the GPU's registers (0xcac00000, 512 KiB) and its line,
 * 75: a node U-Boot's tree leaves out, which a quirk adds from the
 * vendor's x1.dtsi. Its power is power's set "gpu": the K1's GPU domain,
 * its clock at 614.4 MHz and its reset released. hwdb votes for it
 * before the lease (a quirk), so it stays on until a dead driver's GPU
 * has been stopped. The rest is done from the loop:
 *
 * 1. The core's ID: 36.29.52.182, or it isn't the GPU this knows.
 * 2. The firmware, once dos can give it (fw.c): loaded, its structures
 *    made, the MIPS started, and a health check through the kernel CCB.
 *
 * Clients connect to "gpu.0", as on the CIX. What a client lends is
 * mapped into the firmware's heap, where its ring and sync object are
 * read and written; GPU_MAP says where (gm_Device). There's no doorbell
 * (gm_Doorbell is 0): the client asks GPU_KICK (queue.c). What a client
 * had goes with its connection.
 */
#include <stddef.h>

#include <planar/devices/gpu.h>
#include <planar/lease.h>
#include <planar/syscalls.h>

#include "rogue.h"

void *memset(void *dst, int c, size_t n);

constexpr int64_t SIG_NAP = 24;
constexpr uint64_t REGS = 0xcac00000, REGS_SIZE = 0x80000;
constexpr uint64_t RETRY = 500 * MS;

struct gpu gpu;

static struct drv *the;
static struct drv_dev *dev0;
static bool checked;

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

/* --- Bring-up. --- */

static bool identify(void)
{
    uint64_t id = gr64(CR_CORE_ID);

    if (id != BVNC) {
        say("not the BXE-2-32 this driver knows: core ", id, 16, "");
        return false;
    }
    glog("core 36.29.52.182, a BXE-2-32");
    return true;
}

static void bring_up(struct drv *d)
{
    int r;

    if (gpu.up || gpu.broken || CurrentTime() < gpu.next_try)
        return;
    gpu.next_try = CurrentTime() + RETRY;
    if (!checked) {
        if (!identify()) {
            gpu.broken = true;
            return;
        }
        checked = true;
    }
    if ((r = fw_boot(d)) == ERR_NOTFOUND)
        return;                         /* dos, or its file, isn't there yet */
    if (r != 0 || !fw_health_check()) {
        glog("the firmware didn't start: given up");
        gpu.up = false;
        gpu.broken = true;
        return;
    }
    say("MIPS firmware up: queues for ", GPU_QUEUES, 10, " clients");
}

/* --- Clients. --- */

static struct client *client_of(unsigned c)
{
    return c < GPU_CLIENTS ? &gpu.clients[c] : nullptr;
}

static void unmap_all(struct client *c)
{
    bool any = false;

    for (unsigned i = 0; i < GPU_MAPS; i++)
        if (c->maps[i].size) {
            heap_unmap(c->maps[i].fw, c->maps[i].size);
            any = true;
        }
    if (any && gpu.up && !gpu.broken)
        fw_mmu_flush();
    for (unsigned i = 0; i < GPU_MAPS; i++)
        if (c->maps[i].size) {
            ReleaseArea((void *)c->maps[i].addr);
            c->maps[i] = (struct map){ 0 };
        }
}

static int64_t map(struct client *c, uint64_t addr, uint64_t size, uint64_t *device)
{
    struct AreaInfo ai;
    struct map *m = nullptr;
    uint32_t fw;

    if (QueryArea((void *)addr, &ai) != 0)
        return ERR_NOTFOUND;            /* not shared with the server */
    if ((uint64_t)ai.ai_Address != addr || ai.ai_Size != size || ai.ai_Area)
        return ERR_BADARG;              /* a whole Area, and not the server's own */
    if (!(ai.ai_Flags & AREAF_NC) || !(ai.ai_Rights & AREAR_WRITE))
        return ERR_RIGHTS;              /* the GPU doesn't see the CPU's caches */
    for (unsigned i = 0; i < GPU_MAPS; i++) {
        if (c->maps[i].size && c->maps[i].addr == addr)
            return ERR_EXISTS;
        if (!c->maps[i].size && !m)
            m = &c->maps[i];
    }
    if (!m)
        return ERR_FULL;
    for (uint64_t o = 0; o < size; o += PAGE)
        if (!bus_of(addr + o))
            return ERR_RIGHTS;          /* where the GPU can't reach */
    if (!(fw = heap_map(addr, size)))
        return ERR_NOMEM;
    if (!fw_mmu_flush()) {
        heap_unmap(fw, size);
        return ERR_TIMEOUT;
    }
    *m = (struct map){ .addr = addr, .size = size, .fw = fw };
    *device = fw;
    return 0;
}

static int64_t unmap(struct client *c, uint64_t addr)
{
    for (unsigned i = 0; i < GPU_MAPS; i++)
        if (c->maps[i].size && c->maps[i].addr == addr) {
            if (c->queued)
                return ERR_BUSY;        /* its queue may be using it */
            heap_unmap(c->maps[i].fw, c->maps[i].size);
            fw_mmu_flush();
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
    if (!gpu.up || gpu.broken) {
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
        *c = (struct client){ .open = true, .task = m->gm_Task };
        m->gm_Server = FindTask(nullptr);
        m->gm_Kind = GPU_ROGUE;
        m->gm_Id = 36u << 24 | 29u << 16 | 52u << 8 | 182u;   /* its BVNC */
        m->gm_Cores = 1;                /* one cluster */
        m->gm_Slots = GPU_QUEUES;
        m->gm_Free = GPU_QUEUES - gpu.nqueues;
        m->gm_Regs = 0;
        break;
    case GPU_MAP:
        e = map(c, m->gm_Address, m->gm_Size, &m->gm_Device);
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
        e = queue_start(d, c, m->gm_Ring, m->gm_RingSize, &m->gm_Input, &m->gm_Output);
        m->gm_Doorbell = 0;             /* none: GPU_KICK */
        break;
    case GPU_KICK:
        e = queue_kick(c);
        break;
    case GPU_STATUS:
        m->gm_Fault = c->fault;
        m->gm_FaultAddress = c->fault_addr;
        m->gm_Faults = c->faults;
        break;
    case GPU_WAIT:
        break;                          /* the firmware interrupts at every update anyway */
    case GPU_EVENT:
        if (c->event) {
            e = ERR_EXISTS;
            break;
        }
        c->event = msg;                 /* answered by tell(), at a context reset */
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
    queue_stop(d, c);                   /* first: nothing reads its memory */
    unmap_all(c);
    *c = (struct client){ 0 };
}

/* --- The loop. --- */

static void interrupt(struct drv *, struct drv_dev *)
{
    while (gr(CR_MIPS_IRQ_STATUS) & 1) {
        gw(CR_MIPS_IRQ_CLEAR, 1);
        if (!gpu.up)
            continue;
        fw_events();
        queue_events();
    }
}

static bool poll(struct drv *d)
{
    bring_up(d);
    /* A queue whose context was reset is stopped, and its client hears
     * of it, if it's listening. */
    for (unsigned i = 0; i < GPU_CLIENTS; i++) {
        struct client *c = &gpu.clients[i];

        if (c->open)
            tell(d, c);
        if (c->open && c->fault && c->queued)
            queue_stop(d, c);
    }
    return false;
}

static uint64_t deadline(struct drv *)
{
    return gpu.up || gpu.broken ? 0 : gpu.next_try;
}

static int bind(struct drv *d, struct drv_dev *dev)
{
    the = d;
    dev0 = dev;
    if (dev->info->dd_Base != REGS || dev->info->dd_Size < REGS_SIZE) {
        glog("not the K1's GPU: its registers aren't leased");
        return DRV_DECLINE;
    }
    gpu.regs = (volatile uint8_t *)REGS;
    gpu.regs_pa = REGS;
    gw(CR_MIPS_IRQ_CLEAR, 1);           /* before the line's bound */
    gpu.next_try = CurrentTime();
    return DRV_OK;
}

static int start(struct drv *d, struct drv_dev *dev)
{
    int64_t e = drv_publish(d, dev, "gpu");

    return e < 0 ? (int)e : 0;
}

static const struct drv_ops ops = {
    .name = "gpu-rogue", .bind = bind, .start = start, .interrupt = interrupt,
    .message = message, .message_size = sizeof(struct GpuMsg), .poll = poll,
    .deadline = deadline, .gone = gone,
};

int main(uint64_t token)
{
    drv_main(token, &ops);
}
