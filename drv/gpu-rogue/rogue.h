/*
 * drv/gpu-rogue/rogue.h -- the BXE-2-32's registers and the firmware's
 * interface, as Linux's imagination driver has them (pvr_rogue_cr_defs.h,
 * pvr_rogue_fwif.h; the offsets are pvr_rogue_fwif_check.h's)
 *
 * The firmware's structures are written by offset, not as C structs:
 * they're the firmware's, in uncached memory, and only a few fields of
 * each are the host's.
 */
#ifndef DRV_GPU_ROGUE_H
#define DRV_GPU_ROGUE_H

#include <stdint.h>

#include <planar/drv.h>

constexpr uint64_t MS = 1000000;
constexpr uint64_t PAGE = 4096;
constexpr unsigned GPU_CLIENTS = DRV_CLIENTS;
constexpr unsigned GPU_MAPS = 16;           /* Areas a client lends at once */
constexpr unsigned GPU_QUEUES = 7;          /* as the CIX has: one per client */

constexpr uint64_t BVNC = 0x0024001d003400b6ull;   /* 36.29.52.182 */
constexpr uint32_t CORE_CLOCK = 614400000;         /* power sets it (drv/power/k1.c) */

/* --- Registers (pvr_rogue_cr_defs.h). --- */

enum : uint32_t {
    CR_CORE_ID = 0x0020,                    /* 64 bits: the BVNC, packed */
    CR_SOFT_RESET = 0x0100, CR_SOFT_RESET2 = 0x0108,
    CR_SIDEKICK_IDLE = 0x03c8,
    CR_MIPS_WRAPPER_CONFIG = 0x0810,
    CR_MIPS_REMAP1_CONFIG1 = 0x0818,        /* remap n's at + 0x10 * (n - 1) */
    CR_MIPS_REMAP1_CONFIG2 = 0x0820,
    CR_MIPS_REMAP5_CONFIG1 = 0x0858, CR_MIPS_REMAP5_CONFIG2 = 0x0860,
    CR_MIPS_IRQ_STATUS = 0x08a8, CR_MIPS_IRQ_CLEAR = 0x08b0,
    CR_MIPS_DEBUG_CONFIG = 0x08c8, CR_MIPS_EXCEPTION_STATUS = 0x08d0,
    CR_MIPS_WRAPPER_STATUS = 0x08e8,
    CR_MTS_SCHEDULE = 0x0b00,
    CR_MTS_BGCTX_THREAD0 = 0x0b30, CR_MTS_BGCTX_THREAD1 = 0x0b38,
    CR_MTS_INTCTX_THREAD0 = 0x0b40, CR_MTS_INTCTX_THREAD1 = 0x0b48,
    CR_MTS_GARTEN_WRAPPER_CONFIG = 0x0b50,
    CR_SLC_CTRL_MISC = 0x3800,
    CR_AXI_ACE_LITE_CONFIGURATION = 0x38c0,
    CR_SYS_BUS_SECURE = 0xa100,
};

/* SOFT_RESET: everything (the PBE2-in-XE mask), the Rascal and Dusts
 * (pvr_rogue_defs.h), and what's left held while the MIPS is loaded. */
constexpr uint64_t RESET_ALL = 0xffeffffffffffc3dull;
constexpr uint64_t RESET_RASCALDUSTS = 0x1f8e0000000ull;
constexpr uint64_t RESET_GARTEN = 0x100000000ull;
constexpr uint64_t RESET2_ALL = 0x1fffff;           /* XE_TPU2's */

/* --- The MIPS's addresses (pvr_rogue_mips.h). --- */

constexpr uint32_t FW_HEAP = 0xc0000000;    /* the firmware's heap: 16 MiB, mapped by its page table */
constexpr uint32_t FW_HEAP_SIZE = 1u << 24;
constexpr uint32_t FW_HEAP_PAGES = FW_HEAP_SIZE / PAGE;
constexpr uint32_t FW_HEAP_MAIN = 0xed0000; /* objects below; config objects above */
constexpr uint32_t FW_CONNECTION = 0xfd0000, FW_OSINIT = 0xfe0000, FW_SYSINIT = 0xff0000;
constexpr uint32_t MIPS_REGS = 0xcf800000;
constexpr uint32_t MIPS_BOOT_CODE = 0x1fc00000, MIPS_BOOT_DATA = 0x1fc01000,
                   MIPS_EXCEPTIONS = 0x1fc02000;
constexpr uint32_t MIPS_PT_PAGES = 4;
/* A page-table entry: the bus address's bits 35:12 at 29:6, uncached
 * (policy 2), dirty (writable), valid, global. */
constexpr uint32_t MIPS_PTE_FLAGS = 2u << 3 | 0x4 | 0x2 | 0x1;

/* --- The firmware's interface (pvr_rogue_fwif.h). --- */

enum : uint32_t {               /* the kernel CCB's commands */
    KCCB_KICK = 0x2abc0065, KCCB_MMUCACHE = 0x2abc0066, KCCB_CLEANUP = 0x2abc006a,
    KCCB_HEALTH_CHECK = 0x2abc0073,
};
enum : uint32_t {               /* a client CCB's */
    CCB_NULL = 0x2abc80d2, CCB_UPDATE = 0x2abc00d5, CCB_PADDING = 0x2abc00dd,
};
enum : uint32_t {               /* the firmware CCB's */
    FWCCB_CONTEXT_RESET = 0x2abc0069, FWCCB_REQUEST_GPU_RESTART = 0x2abc006d,
};
constexpr uint32_t KCCB_CMD = 88, KCCB_N = 128, FWCCB_N = 32;
constexpr uint32_t CCB_HEADER = 40;
constexpr uint32_t RTN_EXECUTED = 1u << 0, RTN_CLEANUP_BUSY = 1u << 1;
constexpr uint32_t MMUCACHE_ALL = 0x0400001f;   /* PT, PD, PC, TLB, and an interrupt */

enum : uint32_t {               /* rogue_fwif_ccb_ctl: the kernel's and firmware's CCBs */
    CTL_WRITE = 0, CTL_READ = 128, CTL_WRAP = 132, CTL_CMD_SIZE = 136, CTL_SIZE = 144,
};
enum : uint32_t {               /* rogue_fwif_cccb_ctl: a client's */
    CCTL_WRITE = 0, CCTL_READ = 4, CCTL_DEP = 8, CCTL_WRAP = 12, CCTL_SIZE = 32,
};
enum : uint32_t {               /* rogue_fwif_sysinit */
    SI_FAULT_PHYS = 0, SI_PDS_EXEC = 8, SI_USC_EXEC = 16, SI_RUNTIME_CFG = 316,
    SI_TRACE = 320, SI_SYSDATA = 324, SI_GPU_UTIL = 328, SI_CLOCK = 344,
    SI_STARTED = 352, SI_MARKER = 356, SI_SIZE = 544,
};
enum : uint32_t {               /* rogue_fwif_osinit */
    OI_KCCB_CTL = 0, OI_KCCB = 4, OI_KCCB_RTN = 8, OI_FWCCB_CTL = 12, OI_FWCCB = 16,
    OI_HWRINFO = 28, OI_OSDATA = 36, OI_HW_BVNC = 40, OI_FW_BVNC = 56, OI_CHECKS_UPDATED = 96,
    OI_SIZE = 104,
};
enum : uint32_t {               /* rogue_fwif_runtime_cfg */
    RC_PM_LATENCY_PERSIST = 8, RC_CLOCK = 12, RC_DUSTS = 16, RC_SIZE = 72,
};
enum : uint32_t {               /* rogue_fwif_tracebuf */
    TB_LOG_TYPE = 0, TB_SPACE = 8, TB_SPACE_SIZE = 424, TB_SPACE_BUF = 4, TB_DWORDS = 856,
    TB_SIZE = 864, TB_BUF_DWORDS = 12000,
};
enum : uint32_t {               /* rogue_fwif_osdata */
    OD_KCCB_EXECUTED = 568, OD_POWER_SYNC = 572, OD_SIZE = 584,
};
enum : uint32_t {               /* rogue_fwif_fwcommoncontext */
    CC_CCB_CTL = 0, CC_CCB = 4, CC_STATE = 24, CC_DM = 60, CC_MAX_DEADLINE = 104,
    CC_MEM_CONTEXT = 136, CC_ID = 140, CC_SIZE = 152,
};
enum : uint32_t {               /* rogue_fwif_fwmemcontext */
    MC_PC = 0, MC_CAT_BASE_SET = 8, MC_SIZE = 32,
};
enum : uint32_t {               /* rogue_mipsfw_boot_data, at the boot data page's start */
    BD_STACK = 0, BD_REG_BASE = 8, BD_PT = 16, BD_PT_LOG2 = 48, BD_PT_PAGES = 52,
    BD_STAGE = 0x400,           /* how far the firmware's boot got */
};
constexpr uint32_t SYSDATA_SIZE = 3624, HWRINFO_SIZE = 2336, GPU_UTIL_SIZE = 10280;
constexpr uint32_t CONNECTION_SIZE = 16;

/* --- The driver. --- */

/* A firmware object: uncached memory, mapped into the firmware's heap. */
struct fwobj {
    uint8_t *cpu;
    uint32_t fw;                    /* its address to the MIPS */
    uint32_t size;                  /* whole pages */
};

struct map {
    uint64_t addr, size;
    uint32_t fw;                    /* where the firmware sees it */
};

/* A client: what it lent, mapped into the firmware's heap; its queue. */
struct client {
    bool open;
    Handle task;
    struct map maps[GPU_MAPS];
    bool queued;
    struct fwobj ctx;               /* its common context, memory context and their states */
    struct fwobj ctl;               /* its client CCB's control */
    void *in, *out;                 /* the pages GPU_QUEUE gave: struct GpuQueueIn, Out */
    uint64_t ring, ring_size;
    uint32_t ring_fw;
    uint32_t last_read;             /* the CCB's read offset when gq_Extract was moved */
    uint64_t sync;                  /* woken when the firmware interrupts */
    uint32_t seen;                  /* its value then */
    uint32_t fault, faults;
    uint64_t fault_addr;
    struct Message *event;          /* GPU_EVENT, held until a context reset */
    unsigned event_ci;              /* its client number, for drv_reply */
    uint32_t told;                  /* faults GPU_EVENT has answered for */
};

struct gpu {
    volatile uint8_t *regs;
    uint64_t regs_pa;
    bool up, broken;
    uint64_t next_try;
    uint32_t *pt;                   /* the MIPS's page table, 4 pages */
    struct fwobj code, data;
    struct fwobj kccb_ctl, kccb, kccb_rtn, fwccb_ctl, fwccb;
    struct fwobj sync;              /* power_sync, mmucache_sync */
    struct fwobj sysinit, osinit, connection;
    uint32_t kccb_sent;             /* commands written, ever */
    uint32_t mmu_seq;
    unsigned nqueues;
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

static inline uint64_t gr64(uint32_t off)
{
    return *(volatile uint64_t *)(gpu.regs + off);
}

static inline void gw64(uint32_t off, uint64_t v)
{
    *(volatile uint64_t *)(gpu.regs + off) = v;
}

static inline void put32(void *obj, uint32_t off, uint32_t v)
{
    *(volatile uint32_t *)((uint8_t *)obj + off) = v;
}

static inline void put64(void *obj, uint32_t off, uint64_t v)
{
    *(volatile uint64_t *)((uint8_t *)obj + off) = v;
}

static inline uint32_t get32(const void *obj, uint32_t off)
{
    return *(const volatile uint32_t *)((const uint8_t *)obj + off);
}

/* Where the GPU sees memory the CPU has at 'pa' (the K1's dram_range1):
 * the first 2 GiB where they are, and from 4 GiB on 2 GiB lower. 0 if it
 * can't: 2-4 GiB isn't memory. */
static inline uint64_t bus_of(uint64_t pa)
{
    if (pa < 0x80000000ull)
        return pa;
    return pa >= 0x100000000ull ? pa - 0x80000000ull : 0;
}

void nap(uint64_t ms);
void say(const char *a, uint64_t v, unsigned base, const char *b);
void glog(const char *text);

/* The firmware (fw.c). */
int fw_boot(struct drv *d);         /* from the loop: 0, ERR_NOTFOUND (no file yet), or failed */
bool fw_obj(struct drv *d, uint32_t size, struct fwobj *o);
void fw_obj_free(struct fwobj *o);
uint32_t heap_map(uint64_t pa, uint64_t size);     /* a firmware address; 0 if no room */
void heap_unmap(uint32_t fw, uint64_t size);
bool fw_mmu_flush(void);
bool fw_health_check(void);
void fw_dump(uint32_t slot);            /* the firmware's page-table walk sees the changes */
bool kccb_send(const uint32_t *cmd, unsigned words, uint32_t *slot);
bool kccb_wait(uint32_t slot, uint64_t ms, uint32_t *rtn);
void fw_events(void);               /* its interrupt: the firmware CCB */

/* Queues (queue.c). */
int64_t queue_start(struct drv *d, struct client *c, uint64_t ring, uint64_t size,
                    uint64_t *input, uint64_t *output);
int64_t queue_kick(struct client *c);
void queue_stop(struct drv *d, struct client *c);
void queue_events(void);
uint32_t fw_addr_of(const struct client *c, uint64_t addr);  /* a client's address, to the firmware */

#endif /* DRV_GPU_ROGUE_H */
