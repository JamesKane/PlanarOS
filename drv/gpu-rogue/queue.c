/*
 * drv/gpu-rogue/queue.c -- a client's queue: a firmware context with
 * the client's ring as its client CCB (Linux's pvr_queue.c, pvr_cccb.c
 * and pvr_context.c; the DDK's rgxkicksync.c for the job)
 *
 * Rogue has no doorbell an app can ring: a client CCB runs when the host
 * kicks it, by a kernel CCB command and a write to MTS_SCHEDULE, which
 * are the server's. So the app writes commands into its ring, which the
 * firmware reads through its heap, and asks GPU_KICK; the server sends
 * the kick with the ring's new write offset (§5.1's server queue). The
 * client CCB's control, which the firmware moves, is the server's: it
 * copies how far the firmware has read into the output page.
 *
 * The context is on the GP data master, which runs commands that need
 * no hardware: V5's job is a NULL command and an UPDATE of the client's
 * sync object (a UFO: the firmware writes 32 bits, then interrupts). Its
 * memory context has an empty page catalogue: such a job reads no GPU
 * memory, and the GPU's own MMU, three levels with a 40-bit VA, comes
 * with jobs that do (V6).
 *
 * A context is let go by the firmware's cleanup command, which says
 * when it's no longer using it (retried while it's busy): only then is
 * what the client lent unmapped.
 */
#include <stddef.h>

#include <planar/devices/gpu.h>
#include <planar/lease.h>
#include <planar/syscalls.h>

#include "rogue.h"

/* In the context object: the common context, the memory context, the
 * context's state (zero, as the GP DM's is), and the page catalogue. */
constexpr uint32_t CTX_MEM = 256, CTX_STATE = 512, CTX_PC = PAGE, CTX_SIZE = 2 * PAGE;
constexpr uint32_t CLEANUP_FWCOMMONCONTEXT = 0;

static Handle area_of(void *p)
{
    struct AreaInfo ai;

    return p && QueryArea(p, &ai) == 0 ? ai.ai_Area : 0;
}

static void area_free(void *p)
{
    Handle a = area_of(p);

    if (a)
        FreeArea(a);
}

uint32_t fw_addr_of(const struct client *c, uint64_t addr)
{
    for (unsigned i = 0; i < GPU_MAPS; i++)
        if (c->maps[i].size && addr >= c->maps[i].addr && addr < c->maps[i].addr + c->maps[i].size)
            return c->maps[i].fw + (uint32_t)(addr - c->maps[i].addr);
    return 0;
}

static void let_go(struct client *c)
{
    if (c->task) {
        if (c->in)
            UnshareArea(area_of(c->in), c->task);
        if (c->out)
            UnshareArea(area_of(c->out), c->task);
    }
    area_free(c->in);
    area_free(c->out);
    fw_obj_free(&c->ctx);
    fw_obj_free(&c->ctl);
    fw_mmu_flush();
    c->in = c->out = nullptr;
    if (c->queued)
        gpu.nqueues--;
    c->queued = false;
}

int64_t queue_start(struct drv *d, struct client *c, uint64_t ring, uint64_t size,
                    uint64_t *input, uint64_t *output)
{
    uint8_t *x;
    unsigned id = (unsigned)(c - gpu.clients);

    if (c->queued)
        return ERR_EXISTS;
    if (gpu.nqueues >= GPU_QUEUES)
        return ERR_FULL;
    c->queued = true;
    gpu.nqueues++;
    if (!fw_obj(d, CTX_SIZE, &c->ctx) || !fw_obj(d, CCTL_SIZE, &c->ctl) ||
        AllocDmaArea(drv_lease(d), PAGE, AREAF_NC, &c->in) < 0 ||
        AllocDmaArea(drv_lease(d), PAGE, AREAF_NC, &c->out) < 0) {
        let_go(c);
        return ERR_NOMEM;
    }
    c->ring = ring;
    c->ring_size = size;
    c->ring_fw = fw_addr_of(c, ring);
    c->last_read = 0;
    c->seen = c->sync ? *(volatile uint32_t *)c->sync : 0;

    put32(c->ctl.cpu, CCTL_WRAP, (uint32_t)size - 1);
    x = c->ctx.cpu;
    put32(x, CC_CCB_CTL, c->ctl.fw);
    put32(x, CC_CCB, c->ring_fw);
    put32(x, CC_STATE, c->ctx.fw + CTX_STATE);
    put32(x, CC_DM, 0);                         /* GP */
    put32(x, CC_MAX_DEADLINE, 30000);
    put32(x, CC_MEM_CONTEXT, c->ctx.fw + CTX_MEM);
    put32(x, CC_ID, id);                        /* what a reset names */
    put64(x + CTX_MEM, MC_PC, bus_of((uint64_t)(x + CTX_PC)));
    put32(x + CTX_MEM, MC_CAT_BASE_SET, 0xffffffff);    /* none yet: the firmware's to set */
    drv_io_barrier();
    if (!fw_mmu_flush()) {
        let_go(c);
        return ERR_TIMEOUT;
    }

    if (ShareArea(area_of(c->in), c->task, AREAR_READ | AREAR_WRITE) != 0 ||
        ShareArea(area_of(c->out), c->task, AREAR_READ) != 0) {
        queue_stop(d, c);
        return ERR_RIGHTS;
    }
    *input = (uint64_t)c->in;
    *output = (uint64_t)c->out;
    return 0;
}

int64_t queue_kick(struct client *c)
{
    struct GpuQueueIn *in = c->in;
    uint64_t insert = in->gq_Insert;
    uint32_t cmd[8];

    if (!c->queued)
        return ERR_BADARG;
    if (insert % 8)
        return ERR_BADARG;              /* commands are 8-byte aligned */
    cmd[0] = KCCB_KICK;
    cmd[1] = 0;
    cmd[2] = c->ctx.fw;
    cmd[3] = (uint32_t)(insert & (c->ring_size - 1));
    cmd[4] = (uint32_t)c->ring_size - 1;
    cmd[5] = 0;                         /* no cleanup controls */
    cmd[6] = cmd[7] = 0;
    return kccb_send(cmd, 8, nullptr) ? 0 : ERR_BUSY;
}

void queue_stop(struct drv *, struct client *c)
{
    uint32_t cmd[4] = { KCCB_CLEANUP, 0, CLEANUP_FWCOMMONCONTEXT, c->ctx.fw };
    uint64_t until = CurrentTime() + 1000 * MS;
    uint32_t slot, rtn = 0;

    if (!c->queued)
        return;
    if (gpu.up && !gpu.broken && c->ctx.fw) {
        do {
            if (!kccb_send(cmd, 4, &slot) || !kccb_wait(slot, 1000, &rtn)) {
                glog("the firmware didn't clean a context up: given up");
                gpu.broken = true;
                break;
            }
            if (rtn & RTN_CLEANUP_BUSY)
                nap(1);
        } while (rtn & RTN_CLEANUP_BUSY && CurrentTime() < until);
        if (rtn & RTN_CLEANUP_BUSY) {
            glog("a context stayed busy: given up");
            gpu.broken = true;
        }
    }
    let_go(c);
}

/* The firmware interrupted: how far each ring is read, and each sync
 * object that moved, woken. */
void queue_events(void)
{
    for (unsigned i = 0; i < GPU_CLIENTS; i++) {
        struct client *c = &gpu.clients[i];
        struct GpuQueueOut *out = c->out;
        uint32_t r, v;

        if (!c->open || !c->queued)
            continue;
        r = get32(c->ctl.cpu, CCTL_READ);
        out->gq_Extract += (r - c->last_read) & (uint32_t)(c->ring_size - 1);
        c->last_read = r;
        if (c->sync && (v = *(volatile uint32_t *)c->sync) != c->seen) {
            c->seen = v;
            WakeAddr((volatile uint64_t *)c->sync, ~0u);
        }
    }
}
