/*
 * drv/gpu-panthor/queue.c -- a client's queue: one group, pinned to a
 * slot, with one stream (Linux's panthor_sched.c: csg_slot_prog_locked,
 * cs_slot_prog_locked, and the job line's handling)
 *
 * In V5 a group never moves: slot n is its client's for as long as the
 * queue stands, with address space n + 1 and doorbell n + 1, as panthor
 * numbers a slot's. So the client rings its stream itself: the doorbell's
 * page is shared with it (ShareLease), and so are the ring's input and
 * output interfaces, a page each, which the MCU sees at its own address
 * (IFACE_VA) and the client at theirs. The ring is the client's own
 * memory, in its address space.
 *
 * A slot is let go by terminating its group (CSG_STATE_TERMINATE): the
 * firmware stops its streams, and only then is the address space taken
 * down and the client's mappings with it.
 */
#include <stddef.h>

#include <planar/lease.h>
#include <planar/syscalls.h>

#include "panthor.h"

void *memset(void *dst, int c, size_t n);

constexpr uint32_t IFACE_VA = 0x05000000;           /* slot n's at + 0x2000 * n */
constexpr uint32_t SUSPEND_VA = 0x06000000;         /* slot n's at + SUSPEND_MAX * n */
constexpr uint32_t SUSPEND_MAX = 0x400000;
constexpr uint32_t PRIORITY = 0xf;

static volatile struct csg_control *csg_ctl(unsigned s)
{
    return (volatile struct csg_control *)((uint8_t *)gpu.shared + CSF_GROUP_CONTROL +
                                           s * gpu.glb->group_stride);
}

static volatile struct cs_control *cs_ctl(unsigned s)
{
    return (volatile struct cs_control *)((uint8_t *)csg_ctl(s) + CSF_STREAM_CONTROL);
}

static volatile struct csg_input *csg_in(unsigned s)
{
    return fw_cpu(csg_ctl(s)->input_va);
}

static volatile struct csg_output *csg_out(unsigned s)
{
    return fw_cpu(csg_ctl(s)->output_va);
}

static volatile struct cs_input *cs_in(unsigned s)
{
    return fw_cpu(cs_ctl(s)->input_va);
}

static volatile struct cs_output *cs_out(unsigned s)
{
    return fw_cpu(cs_ctl(s)->output_va);
}

/* The range of the lease holding 'addr', for ShareLease. */
static Handle lease_of(struct drv *d, uint64_t addr)
{
    for (unsigned k = 0; k < d->info.hm_NRanges; k++) {
        const struct DrvRange *r = &d->info.hm_Range[k];

        if (addr >= r->dr_Base && addr - r->dr_Base < r->dr_Size)
            return r->dr_Lease;
    }
    return 0;
}

static uint64_t doorbell_of(unsigned s)
{
    return (uint64_t)gpu.regs + CSF_DOORBELL0 + 0x10000 * (s + 1);
}

static void area_free(void *p)
{
    struct AreaInfo ai;

    if (p && QueryArea(p, &ai) == 0 && ai.ai_Area)
        FreeArea(ai.ai_Area);
}

static Handle area_of(void *p)
{
    struct AreaInfo ai;

    return QueryArea(p, &ai) == 0 ? ai.ai_Area : 0;
}

/* The group's state asked for and its ack waited for (csgs_upd_ctx). */
static bool csg_ask(unsigned s, uint32_t value, uint32_t mask)
{
    volatile struct csg_input *in = csg_in(s);

    in->req = (in->req & ~mask) | (value & mask);
    gpu.gin->doorbell_req = ((gpu.gout->doorbell_ack ^ 1u << s) & 1u << s) |
                            (gpu.gin->doorbell_req & ~(1u << s));
    fw_ring(0);
    return fw_wait(&in->req, &csg_out(s)->ack, mask, 100);
}

static void slot_free(struct drv *d, unsigned s)
{
    struct slot *sl = &gpu.slots[s];
    struct client *c = sl->owner;

    if (c && c->task) {
        UnshareLease(lease_of(d, doorbell_of(s)), doorbell_of(s), c->task);
        UnshareArea(area_of(sl->in), c->task);
        UnshareArea(area_of(sl->out), c->task);
    }
    if (sl->iface_va)
        vm_unmap(&gpu.mcu, sl->iface_va, 2 * PAGE);
    area_free(sl->in);
    area_free(sl->out);
    if (sl->suspend) {
        vm_unmap(&gpu.mcu, sl->suspend_va, (csg_ctl(s)->suspend_size + PAGE - 1) & ~(PAGE - 1));
        area_free(sl->suspend);
    }
    *sl = (struct slot){ 0 };
    if (c)
        c->slot = -1;
}

int64_t queue_start(struct drv *d, struct client *c, uint64_t ring, uint64_t size,
                    uint64_t *input, uint64_t *output, uint64_t *doorbell)
{
    struct slot *sl;
    volatile struct csg_input *gi;
    volatile struct cs_input *si;
    uint32_t suspend;
    unsigned s = 0;

    if (c->slot >= 0)
        return ERR_EXISTS;
    while (s < gpu.nslots && gpu.slots[s].owner)
        s++;
    if (s == gpu.nslots)
        return ERR_FULL;
    sl = &gpu.slots[s];
    sl->owner = c;
    c->slot = (int)s;
    suspend = (csg_ctl(s)->suspend_size + PAGE - 1) & ~(PAGE - 1);
    if (suspend > SUSPEND_MAX || !csg_in(s) || !csg_out(s) || !cs_in(s) || !cs_out(s)) {
        glog("a group slot's interfaces are out of reach");
        slot_free(d, s);
        return ERR_BADARG;
    }

    /* The ring's interfaces, a page each (an Area each: the input
     * shared writable, the output read-only), and the suspend buffer. */
    sl->iface_va = IFACE_VA + 0x2000 * s;
    if (AllocDmaArea(drv_lease(d), PAGE, AREAF_NC, &sl->in) < 0 ||
        AllocDmaArea(drv_lease(d), PAGE, AREAF_NC, &sl->out) < 0) {
        slot_free(d, s);
        return ERR_NOMEM;
    }
    if (!vm_map(&gpu.mcu, sl->iface_va, (uint64_t)sl->in, PAGE, true, false) ||
        !vm_map(&gpu.mcu, sl->iface_va + PAGE, (uint64_t)sl->out, PAGE, true, false)) {
        slot_free(d, s);
        return ERR_NOMEM;
    }
    if (suspend) {
        if (AllocDmaArea(drv_lease(d), suspend, AREAF_NC, &sl->suspend) < 0) {
            sl->suspend = nullptr;
            slot_free(d, s);
            return ERR_NOMEM;
        }
        sl->suspend_va = SUSPEND_VA + SUSPEND_MAX * s;
        if (!vm_map(&gpu.mcu, sl->suspend_va, (uint64_t)sl->suspend, suspend, true, false)) {
            slot_free(d, s);
            return ERR_NOMEM;
        }
    }
    if (!vm_enable(&c->vm, (int)s + 1)) {
        slot_free(d, s);
        return ERR_BADARG;
    }

    /* Its one stream (cs_slot_prog_locked). */
    si = cs_in(s);
    si->ringbuf_base = ring;
    si->ringbuf_size = (uint32_t)size;
    si->ringbuf_input = sl->iface_va;
    si->ringbuf_output = sl->iface_va + PAGE;
    si->config = 1 | (s + 1) << 8;              /* priority, doorbell */
    si->ack_irq_mask = ~0u;
    si->req = (si->req & ~(CS_IDLE_SYNC_WAIT | CS_IDLE_EMPTY | CS_STATE_MASK | CS_EXTRACT_EVENT)) |
              CS_IDLE_SYNC_WAIT | CS_IDLE_EMPTY | CS_STATE_START | CS_EXTRACT_EVENT;

    /* The group (csg_slot_prog_locked), then started. */
    gi = csg_in(s);
    gi->allow_compute = gpu.cores;
    gi->allow_fragment = gpu.cores;
    gi->allow_other = 1;
    gi->endpoint_req = (uint32_t)__builtin_popcountll(gpu.cores) |
                       (uint32_t)__builtin_popcountll(gpu.cores) << 8 | 1u << 16 | PRIORITY << 28;
    gi->config = s + 1;
    gi->suspend_buf = suspend ? sl->suspend_va : 0;
    gi->protm_suspend_buf = 0;
    gi->ack_irq_mask = ~0u;
    gi->doorbell_req = ((csg_out(s)->doorbell_ack ^ 1u) & 1u) | (gi->doorbell_req & ~1u);
    if (!csg_ask(s, CSG_STATE_START | ((csg_out(s)->ack ^ CSG_ENDPOINT_CONFIG) & CSG_ENDPOINT_CONFIG),
                 CSG_STATE_MASK | CSG_ENDPOINT_CONFIG)) {
        say("group slot ", s, 10, " didn't start");
        vm_disable(&c->vm);
        slot_free(d, s);
        return ERR_TIMEOUT;
    }

    /* The client's: the interfaces, and its doorbell. */
    if (ShareArea(area_of(sl->in), c->task, AREAR_READ | AREAR_WRITE) != 0 ||
        ShareArea(area_of(sl->out), c->task, AREAR_READ) != 0 ||
        ShareLease(lease_of(d, doorbell_of(s)), doorbell_of(s), PAGE, c->task) != 0) {
        queue_stop(d, c);
        return ERR_RIGHTS;
    }
    *input = (uint64_t)sl->in;
    *output = (uint64_t)sl->out;
    *doorbell = doorbell_of(s);
    return 0;
}

void queue_stop(struct drv *d, struct client *c)
{
    unsigned s;

    if (c->slot < 0)
        return;
    s = (unsigned)c->slot;
    if (gpu.up && !csg_ask(s, CSG_STATE_TERMINATE, CSG_STATE_MASK))
        say("group slot ", s, 10, " didn't terminate");
    vm_disable(&c->vm);
    slot_free(d, s);
}

/* --- The job line: the firmware's events. --- */

static void csg_events(unsigned s)
{
    volatile struct csg_input *in = csg_in(s);
    volatile struct csg_output *out = csg_out(s);
    struct client *c = gpu.slots[s].owner;
    uint32_t req = in->req, ack = out->ack, irq_req = out->cs_irq_req, events;

    events = (req ^ ack) & CSG_EVT_MASK;
    if (!events && irq_req == in->cs_irq_ack)
        return;
    in->cs_irq_ack = irq_req;
    in->req = (in->req & ~events) | (ack & events);
    if (irq_req & 1) {                          /* its stream's */
        volatile struct cs_input *si = cs_in(s);
        volatile struct cs_output *so = cs_out(s);
        uint32_t bad = (si->req ^ so->ack) & (CS_FAULT | CS_FATAL);

        if (bad && c) {
            c->fault = bad & CS_FATAL ? so->fatal : so->fault;
            c->fault_addr = bad & CS_FATAL ? so->fatal_info : so->fault_info;
            c->faults++;
            say("group slot ", s, 10, bad & CS_FATAL ? ": a fatal error" : ": a fault");
            say("  exception ", c->fault, 16, "");
        }
        si->req = (si->req & ~bad) | (so->ack & bad);
    }
    if (events & CSG_PROGRESS_TIMER_EVENT && c) {
        c->fault = 1;
        c->faults++;
        say("group slot ", s, 10, " made no progress");
    }
    if (events & CSG_SYNC_UPDATE && c && c->sync)
        WakeAddr((void *)c->sync, ~0u);
    gpu.gin->doorbell_req = ((gpu.gout->doorbell_ack ^ 1u << s) & 1u << s) |
                            (gpu.gin->doorbell_req & ~(1u << s));
    fw_ring(0);
}

void queue_events(void)
{
    uint32_t st = gr(JOB_INT_STAT);

    gw(JOB_INT_CLEAR, st);
    if (!gpu.up)
        return;
    if (st & JOB_INT_GLOBAL) {
        uint32_t events = (gpu.gin->req ^ gpu.gout->ack) & 0x07f00000u;   /* GLB_EVT_MASK */

        gpu.gin->req = (gpu.gin->req & ~events) | (gpu.gout->ack & events);
        if (gpu.gout->halt_status)
            say("the firmware halted: ", gpu.gout->halt_status, 16, "");
    }
    for (unsigned s = 0; s < gpu.nslots; s++)
        if (st & 1u << s)
            csg_events(s);
}
