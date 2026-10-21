/*
 * drv/display/saturn.h -- the K1's HDMI DPU, "saturn" (v5-plan step 3)
 *
 * The vendor kernel's drivers/gpu/drm/ky (saturn_regs), on its online
 * pipeline 2, as omega drove it on this board. Four RDMA channels each
 * fetch a layer; a composer slot shows one, and slot 0 is the top. The
 * RDMA, composer and channel-enable registers are shadowed until
 * CTL2_CFG_RDY, which the hardware clears as it latches them at the
 * start of the next frame. Here for saturn.c and for hwdb's stop method
 * (servers/hwdb/server.c).
 *
 * U-Boot scans out on RDMA 3: that's the console's, always. display
 * moves its slot to the bottom (7), and puts its planes above it.
 * Nothing here touches the clocks, the timing or sw_start: any of those
 * would drop HDMI.
 */
#ifndef DRV_DISPLAY_SATURN_H
#define DRV_DISPLAY_SATURN_H

#include <stdint.h>

constexpr unsigned S_CONSOLE_RDMA = 3;
constexpr unsigned S_SLOTS = 8;

enum : uint32_t {
    S_CTL2_RCH_EN = 0x560,          /* RDMAs on [11:0]; output [18] */
    S_CTL2_CFG_RDY = 0x56c,         /* [0]: latch at the next frame */
    S_CMDLIST_EN = 0x614,           /* + 4n: must be 0, or the cmdlist loads RDMA n */
    S_INT_ENABLE = 0x910,           /* the vendor's "msk": 1 enables */
    S_INT_STATUS = 0x938,           /* write 1 to clear */
    S_CMPS = 0x4c00,                /* composer 2 */
    S_OUT_ACTIVE = 0x1808c,         /* OUTCTRL2: h [13:0], v [29:16] */
};
constexpr uint32_t S_INT_VSYNC = 1u << 0;

/* RDMA n's */
static inline uint32_t S_RDMA(unsigned n) { return 0xa80 + 0x100 * n; }
enum : uint32_t {
    S_LAYER_CTRL = 0x00,            /* U-Boot's 0x00202040: raw, composer 2 */
    S_Y_OFST = 0x04,                /* must be the slot's top */
    S_SCL_RATIO_V = 0x08,
    S_BASE_LOW = 0x20, S_BASE_HIGH = 0x24,     /* 34 bits */
    S_STRIDE = 0x38,
    S_IMG_SIZE = 0x3c,              /* w [15:0], h [31:16] */
    S_CROP_START = 0x40, S_CROP_END = 0x44,    /* x [15:0], y [31:16]; the end inclusive */
    S_ROT_MODE = 0x70,              /* format [5:0] */
    S_AFBC_CFG = 0x74,
    S_LINEBUF = 0x78,               /* size [15:0], start [27:16], 32-byte units; map [28] */
    S_ALPHA01 = 0x98, S_ALPHA23 = 0x9c,
};
constexpr uint32_t S_FMT_ARGB8888 = 4, S_FMT_XRGB8888 = 8;
constexpr uint32_t S_LAYER_CTRL_RAW = 0x00202040;
constexpr uint32_t S_BANK1 = 1116;      /* RDMAs 2 and 3's line buffer, units */

/* MMU TBU k's control: RDMA n uses TBU 2n; 0 is off */
static inline uint32_t S_TBU(unsigned n) { return 0x1780 + 0x40 * 2 * n; }

/* Composer slot n's */
static inline uint32_t S_SLOT(unsigned n) { return S_CMPS + 0x38 + 0x20 * n; }
enum : uint32_t {
    S_SLOT_EN = 0x00,               /* en [0], RDMA [4:1] */
    S_SLOT_LEFT = 0x10,             /* x [23:8] */
    S_SLOT_TOP_RIGHT = 0x14,        /* y [15:0], last x [31:16] */
    S_SLOT_BOTTOM = 0x18,           /* last y [15:0], blend [17:16], pixel alpha [18] */
    S_SLOT_ALPHA = 0x1c,            /* layer alpha [23:16] */
};
constexpr uint32_t S_PIXEL_ALPHA = 1u << 18;

static inline uint32_t s_rd(volatile uint8_t *r, uint32_t off)
{
    return *(volatile uint32_t *)(r + off);
}

static inline void s_wr(volatile uint8_t *r, uint32_t off, uint32_t v)
{
    *(volatile uint32_t *)(r + off) = v;
}

/* The slot showing RDMA 'n', or S_SLOTS. */
static inline unsigned saturn_slot_of(volatile uint8_t *r, unsigned n)
{
    for (unsigned s = 0; s < S_SLOTS; s++) {
        uint32_t en = s_rd(r, S_SLOT(s) + S_SLOT_EN);

        if ((en & 1) && (en >> 1 & 0xf) == n)
            return s;
    }
    return S_SLOTS;
}

/*
 * Every display plane off, latched: each slot showing another RDMA than
 * the console's, and those RDMAs. True once the DPU has let go of their
 * memory; 'spins' bounds the wait, a frame at most.
 */
static inline bool saturn_quiet(volatile uint8_t *r, uint64_t spins)
{
    uint32_t en = s_rd(r, S_CTL2_RCH_EN);

    if (!(en & 1u << S_CONSOLE_RDMA))
        return true;                /* not lit: it reads nothing */
    for (unsigned s = 0; s < S_SLOTS; s++) {
        uint32_t v = s_rd(r, S_SLOT(s) + S_SLOT_EN);

        if ((v & 1) && (v >> 1 & 0xf) != S_CONSOLE_RDMA)
            s_wr(r, S_SLOT(s) + S_SLOT_EN, 0);
    }
    s_wr(r, S_CTL2_RCH_EN, en & ~(0xfffu & ~(1u << S_CONSOLE_RDMA)));
    s_wr(r, S_CTL2_CFG_RDY, 1);
    while (s_rd(r, S_CTL2_CFG_RDY) & 1)
        if (spins-- == 0)
            return false;
    return true;
}

#endif /* DRV_DISPLAY_SATURN_H */
