/*
 * drv/gpu-panthor/panthor.h -- the G720's registers and the CSF's
 * interfaces, as Linux's panthor has them (panthor_regs.h, panthor_fw.h)
 */
#ifndef DRV_GPU_PANTHOR_H
#define DRV_GPU_PANTHOR_H

#include <stdint.h>

#include <planar/drv.h>

constexpr uint64_t MS = 1000000;
constexpr uint64_t PAGE = 4096;
constexpr unsigned GPU_SLOTS = 8;          /* group slots used, at most: one per client */
constexpr unsigned GPU_CLIENTS = DRV_CLIENTS;
constexpr unsigned GPU_MAPS = 16;          /* Areas a client lends at once */

/* --- The GPU's registers, from its second window (panthor_regs.h). --- */

enum : uint32_t {
    GPU_ID = 0x0, GPU_L2_FEATURES = 0x4, GPU_MMU_FEATURES = 0x14, GPU_AS_PRESENT = 0x18,
    GPU_CSF_ID = 0x1c,
    GPU_INT_RAWSTAT = 0x20, GPU_INT_CLEAR = 0x24, GPU_INT_MASK = 0x28, GPU_INT_STAT = 0x2c,
    GPU_CMD = 0x30, GPU_STATUS_REG = 0x34, GPU_FAULT_STATUS = 0x3c, GPU_FAULT_ADDR = 0x40,
    GPU_SHADER_PRESENT = 0x100, GPU_L2_PRESENT = 0x120, L2_READY = 0x160, L2_PWRON = 0x1a0,
    L2_PWRTRANS = 0x220, GPU_REVID = 0x280,
    GPU_COHERENCY_FEATURES = 0x300, GPU_COHERENCY_PROTOCOL = 0x304,
    MCU_CONTROL = 0x700, MCU_STATUS = 0x704,
    JOB_INT_RAWSTAT = 0x1000, JOB_INT_CLEAR = 0x1004, JOB_INT_MASK = 0x1008, JOB_INT_STAT = 0x100c,
    MMU_INT_RAWSTAT = 0x2000, MMU_INT_CLEAR = 0x2004, MMU_INT_MASK = 0x2008, MMU_INT_STAT = 0x200c,
    MMU_AS0 = 0x2400,                       /* address space n's at + 0x40 * n */
};
constexpr uint32_t CSF_DOORBELL0 = 0x80000;     /* doorbell n's page at + 0x10000 * n */

enum : uint32_t {               /* GPU_INT_* */
    GPU_IRQ_FAULT = 1u << 0, GPU_IRQ_RESET_COMPLETED = 1u << 8,
    GPU_IRQ_CLEAN_CACHES_COMPLETED = 1u << 17,
};
enum : uint32_t {               /* GPU_CMD */
    GPU_SOFT_RESET = 1 | 1 << 8,
    GPU_FLUSH_ALL = 4 | (3 << 0 | 3 << 4) << 8,     /* clean and invalidate L2 and LSC */
};
enum : uint32_t { MCU_DISABLE = 0, MCU_AUTO = 2 };
enum : uint32_t { MCU_STATUS_DISABLED = 0, MCU_STATUS_ENABLED = 1, MCU_STATUS_HALT = 2,
                  MCU_STATUS_FATAL = 3 };
constexpr uint32_t COHERENCY_ACE_LITE = 0, COHERENCY_SHAREABLE_CACHE = 1u << 5;
constexpr uint32_t JOB_INT_GLOBAL = 1u << 31;

/* An address space's (MMU_AS0 + 0x40 * n). */
enum : uint32_t {
    AS_TRANSTAB = 0x0, AS_MEMATTR = 0x8, AS_LOCKADDR = 0x10, AS_COMMAND = 0x18,
    AS_FAULTSTATUS = 0x1c, AS_FAULTADDRESS = 0x20, AS_STATUS = 0x28, AS_TRANSCFG = 0x30,
};
enum : uint32_t { AS_CMD_UPDATE = 1, AS_CMD_LOCK = 2, AS_CMD_UNLOCK = 3 };
constexpr uint32_t AS_ACTIVE = 1u << 0;
constexpr uint64_t TRANSCFG_UNMAPPED = 1, TRANSCFG_4K = 6, TRANSCFG_PTW_NC = 1u << 24,
                   TRANSCFG_PTW_RA = 1u << 30;

/* --- The CSF's interfaces, in the shared section (panthor_fw.h). --- */

struct glb_control {
    uint32_t version, features, input_va, output_va, group_num, group_stride, perfcnt_size,
             instr_features;
};

struct glb_input {
    uint32_t req, ack_irq_mask, doorbell_req, reserved1, progress_timer, poweroff_timer;
    uint64_t core_en_mask;
    uint32_t reserved2[24];         /* perfcnt_*: unused */
    uint32_t idle_timer;
};
static_assert(__builtin_offsetof(struct glb_input, idle_timer) == 0x80, "the CSF's layout");

struct glb_output {
    uint32_t ack, reserved1, doorbell_ack, reserved2, halt_status;
};

enum : uint32_t {               /* glb req and ack */
    GLB_HALT = 1u << 0, GLB_CFG_PROGRESS_TIMER = 1u << 1, GLB_CFG_ALLOC_EN = 1u << 2,
    GLB_CFG_POWEROFF_TIMER = 1u << 3, GLB_PING = 1u << 8, GLB_IDLE_EN = 1u << 10,
    GLB_IDLE = 1u << 26,
};

struct csg_control {
    uint32_t features, input_va, output_va, suspend_size, protm_suspend_size, stream_num,
             stream_stride;
};

struct csg_input {
    uint32_t req, ack_irq_mask, doorbell_req, cs_irq_ack, reserved1[4];
    uint64_t allow_compute, allow_fragment;
    uint32_t allow_other, endpoint_req, reserved2[2];
    uint64_t suspend_buf, protm_suspend_buf;
    uint32_t config, iter_trace_config;
};
static_assert(__builtin_offsetof(struct csg_input, config) == 0x50, "the CSF's layout");

struct csg_output {
    uint32_t ack, reserved1, doorbell_ack, cs_irq_req, status_endpoint_current,
             status_endpoint_req, status_state, resource_dep;
};

enum : uint32_t {               /* csg req and ack */
    CSG_STATE_MASK = 7, CSG_STATE_TERMINATE = 0, CSG_STATE_START = 1,
    CSG_ENDPOINT_CONFIG = 1u << 4,
    CSG_SYNC_UPDATE = 1u << 28, CSG_IDLE = 1u << 29, CSG_PROGRESS_TIMER_EVENT = 1u << 31,
    CSG_EVT_MASK = CSG_SYNC_UPDATE | CSG_IDLE | CSG_PROGRESS_TIMER_EVENT,
};

struct cs_control {
    uint32_t features, input_va, output_va;
};

struct cs_input {
    uint32_t req, config, reserved1, ack_irq_mask;
    uint64_t ringbuf_base;
    uint32_t ringbuf_size, reserved2;
    uint64_t heap_start, heap_end, ringbuf_input, ringbuf_output;
    uint32_t instr_config, instrbuf_size;
    uint64_t instrbuf_base, instrbuf_offset_ptr;
};
static_assert(__builtin_offsetof(struct cs_input, ringbuf_input) == 0x30, "the CSF's layout");

struct cs_output {
    uint32_t ack, reserved1[15];
    uint64_t status_cmd_ptr;
    uint32_t status_wait, status_req_resource;
    uint64_t status_wait_sync_ptr;
    uint32_t status_wait_sync_value, status_scoreboards, status_blocked_reason,
             status_wait_sync_value_hi, reserved2[6];
    uint32_t fault, fatal;
    uint64_t fault_info, fatal_info;
};
static_assert(__builtin_offsetof(struct cs_output, fault) == 0x80, "the CSF's layout");

enum : uint32_t {               /* cs req and ack */
    CS_STATE_MASK = 7, CS_STATE_STOP = 0, CS_STATE_START = 1, CS_EXTRACT_EVENT = 1u << 4,
    CS_IDLE_SYNC_WAIT = 1u << 8, CS_IDLE_EMPTY = 1u << 10,
    CS_FATAL = 1u << 30, CS_FAULT = 1u << 31,
};

constexpr uint32_t CSF_GROUP_CONTROL = 0x1000;  /* groups' control, from the shared section */
constexpr uint32_t CSF_STREAM_CONTROL = 0x40;   /* streams', from their group's */

/* --- The driver. --- */

/* An address space's page tables (mmu.c). */
struct vm {
    int as;                         /* its MMU address space; -1 none */
    uint64_t *root;                 /* level 0, 512 entries */
};

struct map {
    uint64_t addr, size;
    Handle area;                    /* the client's, as QueryArea had it */
};

/* A client: its address space, its mappings, its queue on a slot. */
struct client {
    bool open;
    Handle task;
    struct vm vm;
    struct map maps[GPU_MAPS];
    int slot;                       /* its group's; -1 none */
    uint64_t sync;                  /* woken at each sync update */
    uint32_t fault, faults;
    uint64_t fault_addr;
};

/* A group slot (queue.c), with its one stream. */
struct slot {
    struct client *owner;
    void *in, *out;                 /* the ring's input and output interfaces, a page each */
    void *suspend;                  /* its suspend buffer */
    uint32_t iface_va, suspend_va;  /* where the MCU sees them */
};

struct gpu {
    volatile uint8_t *rcsu, *regs, *rst;
    uint32_t id, csf_id, mmu_features, as_present;
    uint64_t cores;
    unsigned va_bits;
    unsigned cs_regs;               /* a stream's work registers */
    bool up;                        /* the firmware booted */
    bool broken;                    /* it failed: given up until restarted */
    uint64_t next_try, next_ping;
    struct vm mcu;
    void *shared;                   /* the shared section, at CSF_SHARED_VA to the MCU */
    uint64_t shared_size;
    volatile struct glb_control *glb;
    volatile struct glb_input *gin;
    volatile struct glb_output *gout;
    unsigned nslots;
    struct slot slots[GPU_SLOTS];
    struct client clients[GPU_CLIENTS];
};

extern struct gpu gpu;

static inline uint32_t gr(uint32_t off)
{
    return *(volatile uint32_t *)(gpu.regs + off);
}

static inline void gw(uint32_t off, uint32_t v)
{
    *(volatile uint32_t *)(gpu.regs + off) = v;
}

static inline void gw64(uint32_t off, uint64_t v)
{
    gw(off, (uint32_t)v);
    gw(off + 4, (uint32_t)(v >> 32));
}

void nap(uint64_t ms);
void say(const char *a, uint64_t v, unsigned base, const char *b);
void glog(const char *text);

/* The firmware (fw.c). */
constexpr uint32_t CSF_SHARED_VA = 0x04000000;
int fw_boot(struct drv *d);                     /* from the loop: 0, ERR_NOTFOUND (no file yet), or failed */
bool fw_wait(volatile uint32_t *req, volatile uint32_t *ack, uint32_t mask, uint64_t ms);
void fw_glb_req(uint32_t value, uint32_t mask);   /* set bits of req */
void fw_glb_toggle(uint32_t mask);                /* toggle them against ack */
void fw_ring(unsigned doorbell);
void fw_ping(void);
void *fw_cpu(uint32_t mcu_va);                    /* in the shared section; nullptr if not */

/* Page tables and address spaces (mmu.c). */
bool mmu_init(struct drv *d);
bool vm_new(struct vm *v);
void vm_free(struct vm *v);
bool vm_map(struct vm *v, uint64_t va, uint64_t pa, uint64_t size, bool write, bool exec);
void vm_unmap(struct vm *v, uint64_t va, uint64_t size);
bool vm_enable(struct vm *v, int as);
void vm_disable(struct vm *v);
void vm_flush(struct vm *v);                      /* after a change: the GPU's walk sees it */
void mmu_interrupt(void);

/* Groups (queue.c). */
int64_t queue_start(struct drv *d, struct client *c, uint64_t ring, uint64_t size,
                    uint64_t *input, uint64_t *output, uint64_t *doorbell);
void queue_stop(struct drv *d, struct client *c);
void queue_events(void);                          /* the job line */

#endif /* DRV_GPU_PANTHOR_H */
