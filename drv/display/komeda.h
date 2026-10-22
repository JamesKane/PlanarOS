/*
 * drv/display/komeda.h -- the CIX's DPU: Arm China's Linlon-D60 (v5-plan step 3)
 *
 * komeda's D71 registers (Linux's d71_regs.h; the vendor's dp_regs.h is
 * the same with a few bits more), in 0x200-byte blocks, the GCU first.
 * Every block's registers are shadowed until GCU_CONFIG_VALID0, which
 * the hardware clears as it latches them at the next frame. Here for
 * komeda.c and for hwdb's stop method (servers/hwdb/server.c), which
 * turns display's planes off when it dies.
 *
 * display's planes are layers the firmware left off, each into its own
 * compiz input above the GOP's: the input is the z-order, and the GOP's
 * path (its layers, scalers and merger) uses the lowest.
 * Nothing here resets the DPU or touches its timing: a reset drops the
 * DisplayPort link for good.
 */
#ifndef DRV_DISPLAY_KOMEDA_H
#define DRV_DISPLAY_KOMEDA_H

#include <stdint.h>

constexpr uint32_t KOMEDA_BLOCK = 0x200;
constexpr unsigned KOMEDA_INPUTS = 5;

/* Each block's */
enum : uint32_t {
    K_BLOCK_INFO = 0x000,           /* id [7:4], type [15:8]; & 0xfff0 is its routing id */
    K_IRQ_RAW = 0x0a0, K_IRQ_CLEAR = 0x0a4, K_IRQ_MASK = 0x0a8,
    K_CONTROL = 0x0d0,
};

/* Block types */
enum : uint32_t {
    K_GCU = 0x00, K_LPU = 0x01, K_CU = 0x02, K_DOU = 0x03,
    K_LAYER = 0x10, K_BS = 0x31,
};

/* The GCU's */
enum : uint32_t {
    K_GLB_CORE_ID = 0x004,          /* product [31:16]: 0x0060 */
    K_GLB_CORE_INFO = 0x008,        /* blocks [7:0] */
    K_GLB_IRQ_STATUS = 0x010,
    K_GCU_CONFIG_VALID0 = 0x0d4,
};
constexpr uint32_t K_GCU_MODE = 0x7;            /* in K_CONTROL: 3, DO0_ACTIVE, when lit */

/* A layer's */
enum : uint32_t {
    K_LAYER_INFO = 0x0c0,
    K_LAYER_FMT = 0x0d8,
    K_IN_SIZE = 0x0e0,              /* w [12:0], h [28:16] */
    K_P0_PTR_LOW = 0x100, K_P0_PTR_HIGH = 0x104, K_P0_STRIDE = 0x108,
    K_AD_CONTROL = 0x160,           /* AFBC: 0, linear */
};
constexpr uint32_t K_L_EN = 1u << 0;
constexpr uint32_t K_ARCACHE = 3u << 28;        /* the firmware's _DSD asks for 3 */
constexpr uint32_t K_FMT_ARGB8888 = 0x08, K_FMT_XRGB8888 = 0x10;

/* The compiz's: input n's */
static inline uint32_t K_CU_INPUT_ID(unsigned n) { return 0x080 + 4 * n; }
static inline uint32_t K_CU_SIZE(unsigned n) { return 0x0e0 + 0x10 * n; }
static inline uint32_t K_CU_OFFSET(unsigned n) { return 0x0e4 + 0x10 * n; }
static inline uint32_t K_CU_CTRL(unsigned n) { return 0x0e8 + 0x10 * n; }
enum : uint32_t { K_CU_BLK_SIZE = 0x0d4 };
constexpr uint32_t K_CU_EN = 1u << 0, K_CU_PAD = 1u << 1;   /* PAD: pixel alpha ignored */

/* The timing controller's (BS) */
enum : uint32_t { K_BS_ACTIVESIZE = 0x0e0 };    /* w [12:0], h [28:16] */
/* PL0's line, and the line the DPU starts fetching a frame at (and takes
 * a commit): the vendor's linlon-dp sets them to 4 and 5. */
enum : uint32_t { K_BS_PROG_LINE = 0x0d4, K_BS_PREFETCH_LINE = 0x0d8, K_BS_VINTERVALS = 0x0e8 };

/* The DOU's interrupts: PL0 is the vblank, on BS's programmed line. */
constexpr uint32_t K_DOU_PL0 = 1u << 13;

static inline uint32_t k_rd(volatile uint8_t *r, uint32_t off)
{
    return *(volatile uint32_t *)(r + off);
}

static inline void k_wr(volatile uint8_t *r, uint32_t off, uint32_t v)
{
    *(volatile uint32_t *)(r + off) = v;
}

/* The block of 'type' and 'id', or 0 (the GCU's own offset, never one). */
static inline uint32_t komeda_find(volatile uint8_t *r, uint32_t type, uint32_t id)
{
    uint32_t n = k_rd(r, K_GLB_CORE_INFO) & 0xff;

    for (uint32_t i = 1; i < n && i < 0x100; i++) {
        uint32_t info = k_rd(r, i * KOMEDA_BLOCK + K_BLOCK_INFO);

        if ((info >> 8 & 0xff) == type && (info >> 4 & 0xf) == id)
            return i * KOMEDA_BLOCK;
    }
    return 0;
}

/* The layer block whose routing id is 'rid', or 0. */
static inline uint32_t komeda_layer_of(volatile uint8_t *r, uint32_t rid)
{
    uint32_t n = k_rd(r, K_GLB_CORE_INFO) & 0xff;

    for (uint32_t i = 1; rid && i < n && i < 0x100; i++) {
        uint32_t info = k_rd(r, i * KOMEDA_BLOCK + K_BLOCK_INFO);

        if ((info >> 8 & 0xff) == K_LAYER && (info & 0xfff0) == (rid & 0xfff0))
            return i * KOMEDA_BLOCK;
    }
    return 0;
}

/*
 * Every display plane off, latched: each compiz input fed by a layer
 * that doesn't show the firmware's framebuffer [fb, fb_end), and its
 * layer. True once the DPU has let go of their memory; 'spins' bounds
 * the wait for the latch, a frame at most. The GOP's own inputs (its
 * scaler's, or a layer on its framebuffer) are left as they are.
 */
static inline bool komeda_quiet(volatile uint8_t *r, uint64_t fb, uint64_t fb_end, uint64_t spins)
{
    uint32_t cu = komeda_find(r, K_CU, 0);

    if (!cu || (k_rd(r, K_CONTROL) & K_GCU_MODE) == 0)
        return true;                /* not lit: it reads nothing */
    for (unsigned n = 0; n < KOMEDA_INPUTS; n++) {
        uint32_t l = komeda_layer_of(r, k_rd(r, cu + K_CU_INPUT_ID(n)));
        uint64_t at;

        if (!l)
            continue;
        at = (uint64_t)k_rd(r, l + K_P0_PTR_HIGH) << 32 | k_rd(r, l + K_P0_PTR_LOW);
        if (at >= fb && at < fb_end)
            continue;
        k_wr(r, l + K_CONTROL, k_rd(r, l + K_CONTROL) & ~K_L_EN);
        k_wr(r, cu + K_CU_INPUT_ID(n), 0);
        k_wr(r, cu + K_CU_CTRL(n), 0);
    }
    k_wr(r, K_GCU_CONFIG_VALID0, 1);
    while (k_rd(r, K_GCU_CONFIG_VALID0) & 1)
        if (spins-- == 0)
            return false;
    return true;
}

#endif /* DRV_DISPLAY_KOMEDA_H */
