/*
 * drv/display/test.c -- display's core and backends on the host
 * (make display-test)
 *
 * Each DPU backend against made-up registers laid out as the firmware
 * leaves them: the CIX's Linlon-D60 with the GOP's scaled path into
 * compiz inputs 0 and 1, the K1's saturn with U-Boot's layer on RDMA 3
 * in slot 0. A thread stands in for the hardware's latch, clearing
 * CONFIG_VALID or CFG_RDY, when a test lets it; frame() is a vblank.
 * Through the core: commits latched at the vblank and not before,
 * clipping, the refusals, a client's going (planes off, buffers freed
 * only after the latch), and the stop method hwdb uses (komeda_quiet,
 * saturn_quiet), which must leave the firmware's plane alone. fb.c, the
 * same core composing in memory.
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <planar/syscalls.h>

#include "display.h"
#include "komeda.h"
#include "saturn.h"

static unsigned checks, failures;

#define CHECK(cond)                                                                 \
    do {                                                                            \
        checks++;                                                                   \
        if (!(cond)) {                                                              \
            failures++;                                                             \
            fprintf(stderr, "%s:%d: failed: %s\n", __FILE__, __LINE__, #cond);      \
        }                                                                           \
    } while (0)

/* --- What main.c gives the core, here made up. --- */

static uint64_t now_ns;
static unsigned live_areas, shares;

int64_t dp_alloc(struct dp_out *, uint64_t size, bool, Handle *area, uint64_t *addr)
{
    void *p = aligned_alloc(4096, size);

    if (!p)
        return ERR_NOMEM;
    memset(p, 0, size);
    *area = (Handle)++live_areas + 1000;
    *addr = (uint64_t)p;
    return 0;
}

void dp_free(Handle)
{
    live_areas--;
}

int64_t dp_share(Handle, Handle task, bool)
{
    if (task == 0)
        return ERR_NOTFOUND;
    shares++;
    return 0;
}

void dp_unshare(Handle, Handle)
{
}

void dp_wake(volatile uint64_t *)
{
}

uint64_t dp_now(void)
{
    return now_ns;
}

void dp_say(const char *text)
{
    printf("  %s\n", text);
}

void dp_clean(const volatile void *, uint64_t)
{
}

int64_t KLog(const char *)
{
    return 0;
}

/* --- The hardware's latch. --- */

static volatile uint8_t *latch_reg;     /* CONFIG_VALID0 or CFG_RDY */
static atomic_bool latching, stop_thread;

static void *hardware(void *)
{
    while (!stop_thread) {
        if (latching && latch_reg && (*(volatile uint32_t *)latch_reg & 1))
            *(volatile uint32_t *)latch_reg = 0;
        struct timespec ts = { 0, 20000 };
        nanosleep(&ts, nullptr);
    }
    return nullptr;
}

/* --- A client of the core. --- */

static struct DisplayMsg msg;

static int64_t ask(struct dp_out *o, unsigned client, uint32_t op)
{
    msg.dm_Op = op;
    msg.dm_Task = 7;
    dp_message(o, client, &msg);
    return msg.dm_Msg.mn_Error;
}

static uint32_t alloc(struct dp_out *o, unsigned client, uint32_t w, uint32_t h, uint32_t f)
{
    memset(&msg, 0, sizeof msg);
    msg.dm_Width = w;
    msg.dm_Height = h;
    msg.dm_Format = f;
    return ask(o, client, DISPLAY_ALLOC) == 0 ? msg.dm_Buffer : 0;
}

static uint64_t commit(struct dp_out *o, unsigned client, uint32_t p, uint32_t id, int32_t x, int32_t y)
{
    memset(&msg, 0, sizeof msg);
    msg.dm_Count = 1;
    msg.dm_Set[0] = (struct DisplayPlane){ p, id, x, y };
    return ask(o, client, DISPLAY_COMMIT) == 0 ? msg.dm_Seq : 0;
}

static uint32_t r32(volatile uint8_t *r, uint32_t off)
{
    return *(volatile uint32_t *)(r + off);
}

static void w32(volatile uint8_t *r, uint32_t off, uint32_t v)
{
    *(volatile uint32_t *)(r + off) = v;
}

/* --- The CIX. --- */

static uint8_t *cix;
static uint32_t fb_words[64 * 64];

enum { LPU = 0x200, L0 = 0x400, L1 = 0x600, L2 = 0x800, L3 = 0xa00, CU = 0xe00, SC0 = 0x1200,
       DOU = 0x1800, BS = 0x1e00 };

static void cix_firmware(void)
{
    volatile uint8_t *r = cix;
    static const struct { uint32_t at, info; } blocks[] = {
        { LPU, 0x0100 }, { L0, 0x1000 }, { L1, 0x1010 }, { L2, 0x1020 }, { L3, 0x1030 },
        { CU, 0x0200 }, { SC0, 0x2100 }, { DOU, 0x0300 }, { BS, 0x3100 },
    };

    memset(cix, 0, 0x20000);
    for (uint32_t i = 1; i < 16; i++)
        w32(r, i * 0x200, 0xff00);  /* reserved */
    for (unsigned i = 0; i < sizeof blocks / sizeof blocks[0]; i++)
        w32(r, blocks[i].at, blocks[i].info);
    w32(r, K_GLB_CORE_ID, 0x0060u << 16);
    w32(r, K_GLB_CORE_INFO, 16 | 2 << 8);
    w32(r, K_CONTROL, 3);
    w32(r, BS + K_CONTROL, 1);
    w32(r, BS + K_BS_ACTIVESIZE, 640 | 480 << 16);
    /* The GOP: layers 0 and 2 on its framebuffer, through a scaler into
     * input 0, and layer 2 straight into input 1. */
    for (uint32_t l = L0; l <= L2; l += L2 - L0) {
        w32(r, l + K_CONTROL, K_ARCACHE | K_L_EN);
        w32(r, l + K_P0_PTR_LOW, (uint32_t)(uint64_t)fb_words);
        w32(r, l + K_P0_PTR_HIGH, (uint32_t)((uint64_t)fb_words >> 32));
    }
    w32(r, CU + K_CU_INPUT_ID(0), 0x2100);
    w32(r, CU + K_CU_CTRL(0), K_CU_EN);
    w32(r, CU + K_CU_INPUT_ID(1), 0x1020);
    w32(r, CU + K_CU_CTRL(1), K_CU_EN);
}

static void cix_frame(void)
{
    w32(cix, K_GCU_CONFIG_VALID0, 0);
    w32(cix, DOU + K_IRQ_RAW, r32(cix, DOU + K_IRQ_RAW) | K_DOU_PL0);
    w32(cix, K_GLB_IRQ_STATUS, 1u << 24);
}

static void test_komeda(void)
{
    struct dp_out o = { .regs = nullptr };
    uint32_t a, c;
    uint64_t seq;

    cix_firmware();
    o.regs = cix;
    o.regs_size = 0x20000;
    o.fb = (volatile uint8_t *)fb_words;
    o.fb_size = sizeof fb_words;
    o.fb_cells[0] = o.fb_cells[1] = o.fb_cells[2] = 64;
    o.fb_cells[3] = 1;
    latch_reg = cix + K_GCU_CONFIG_VALID0;
    latching = true;
    CHECK(dp_start(&o, &dp_komeda, false));
    latching = false;
    CHECK(o.width == 640 && o.height == 480 && o.nplanes == 3);
    CHECK(o.be_words[3 + 1] == L1 && o.be_words[3 + 2] == L3);    /* the layers the GOP left off */
    CHECK(o.plane[2].di_Flags & DISPLAYP_CURSOR);
    CHECK(r32(cix, DOU + K_IRQ_MASK) & K_DOU_PL0);
    CHECK((r32(cix, L2 + K_CONTROL) & K_L_EN) && r32(cix, CU + K_CU_INPUT_ID(1)) == 0x1020);

    /* An overlay, latched at the vblank and not before. */
    a = alloc(&o, 0, 100, 50, DISPLAY_XRGB8888);
    c = alloc(&o, 0, 32, 32, DISPLAY_ARGB8888);
    CHECK(a && c);
    CHECK((seq = commit(&o, 0, 1, a, 10, 20)) == 1);
    CHECK(r32(cix, K_GCU_CONFIG_VALID0) == 1);
    CHECK(r32(cix, CU + K_CU_INPUT_ID(2)) == 0x1010);
    CHECK(r32(cix, CU + K_CU_OFFSET(2)) == (10 | 20 << 16));
    CHECK(r32(cix, CU + K_CU_CTRL(2)) == (K_CU_EN | K_CU_PAD | 0xff00));
    CHECK(r32(cix, L1 + K_IN_SIZE) == (100 | 50 << 16));
    CHECK(r32(cix, L1 + K_LAYER_FMT) == K_FMT_XRGB8888);
    CHECK((r32(cix, L1 + K_P0_STRIDE) & 15) == 0);
    dp_tick(&o);
    CHECK(o.sync->ds_Disp == 0 && o.sync->ds_Vblank == 0);      /* no interrupt yet */
    cix_frame();
    dp_tick(&o);
    CHECK(o.sync->ds_Disp == seq && o.sync->ds_Safe == seq && o.sync->ds_Vblank == 1);
    CHECK(r32(cix, DOU + K_IRQ_CLEAR) & K_DOU_PL0);

    /* The cursor half off the left edge: its right half shown. */
    CHECK((seq = commit(&o, 0, 2, c, -16, 0)) == 2);
    CHECK(r32(cix, L3 + K_IN_SIZE) == (16 | 32 << 16));
    CHECK(r32(cix, CU + K_CU_OFFSET(3)) == 0);
    CHECK(r32(cix, L3 + K_P0_PTR_LOW) == (uint32_t)(o.buf[c - 1].addr + 16 * 4));
    CHECK(r32(cix, L3 + K_LAYER_FMT) == K_FMT_ARGB8888);
    cix_frame();
    dp_tick(&o);
    CHECK(o.sync->ds_Disp == 2);

    /* Refusals. */
    CHECK(commit(&o, 0, 0, a, 0, 0) == 0 && msg.dm_Msg.mn_Error == ERR_RIGHTS);
    CHECK(commit(&o, 1, 1, a, 0, 0) == 0 && msg.dm_Msg.mn_Error == ERR_BUSY);     /* 0's plane */
    CHECK(commit(&o, 0, 1, 99, 0, 0) == 0 && msg.dm_Msg.mn_Error == ERR_NOTFOUND);
    memset(&msg, 0, sizeof msg);
    msg.dm_Buffer = a;
    CHECK(ask(&o, 0, DISPLAY_FREE) == ERR_BUSY);
    CHECK(ask(&o, 1, DISPLAY_FREE) == ERR_NOTFOUND);              /* another's */
    CHECK(alloc(&o, 0, 5000, 10, DISPLAY_XRGB8888) == 0 && msg.dm_Msg.mn_Error == ERR_BADARG);

    /* Two commits before a latch go together; a third waits for it. */
    CHECK(commit(&o, 0, 1, a, 0, 0) == 3);
    CHECK(commit(&o, 0, 1, a, 5, 5) == 4);
    CHECK(o.armed && o.armed_seq == 3 && o.dirty);
    cix_frame();
    dp_tick(&o);
    CHECK(o.sync->ds_Disp == 3 && o.armed && o.armed_seq == 4);
    CHECK(r32(cix, CU + K_CU_OFFSET(2)) == (5 | 5 << 16));
    cix_frame();
    dp_tick(&o);
    CHECK(o.sync->ds_Disp == 4 && !o.armed);

    /* The client goes: planes off at the next latch, buffers freed after. */
    dp_gone(&o, 0);
    CHECK(dp_live_buffers(&o) == 2);
    CHECK(r32(cix, CU + K_CU_INPUT_ID(2)) == 0 && !(r32(cix, L1 + K_CONTROL) & K_L_EN));
    cix_frame();
    dp_tick(&o);
    CHECK(dp_live_buffers(&o) == 0);
    CHECK((r32(cix, L2 + K_CONTROL) & K_L_EN) && r32(cix, CU + K_CU_INPUT_ID(1)) == 0x1020);

    /* hwdb's stop: the planes off, the GOP's left. */
    a = alloc(&o, 1, 64, 64, DISPLAY_XRGB8888);
    CHECK(commit(&o, 1, 1, a, 0, 0) != 0);
    cix_frame();
    dp_tick(&o);
    latching = true;
    CHECK(komeda_quiet(cix, (uint64_t)fb_words, (uint64_t)fb_words + sizeof fb_words, 100000000));
    latching = false;
    CHECK(r32(cix, CU + K_CU_INPUT_ID(2)) == 0 && !(r32(cix, L1 + K_CONTROL) & K_L_EN));
    CHECK(r32(cix, CU + K_CU_INPUT_ID(0)) == 0x2100 && r32(cix, CU + K_CU_INPUT_ID(1)) == 0x1020);
    CHECK(r32(cix, L0 + K_CONTROL) & K_L_EN);

    /* Not lit: declined. */
    w32(cix, K_CONTROL, 0);
    o = (struct dp_out){ .regs = cix, .regs_size = 0x20000, .fb = (volatile uint8_t *)fb_words };
    CHECK(!dp_komeda.take(&o, false));
}

/* --- The K1. --- */

static uint8_t *k1;

static void k1_firmware(void)
{
    memset(k1, 0, 0x2a000);
    w32(k1, S_CTL2_RCH_EN, 1u << 3 | 1u << 18);
    w32(k1, S_OUT_ACTIVE, 1920 | 1080u << 16);
    w32(k1, S_SLOT(0) + S_SLOT_EN, 1 | 3 << 1);
    w32(k1, S_SLOT(0) + S_SLOT_TOP_RIGHT, 1919u << 16);
    w32(k1, S_SLOT(0) + S_SLOT_BOTTOM, 1079);
    w32(k1, S_SLOT(0) + S_SLOT_ALPHA, 0xffu << 16);
    w32(k1, S_RDMA(3) + S_BASE_LOW, (uint32_t)(uint64_t)fb_words);
    w32(k1, S_RDMA(2) + S_LINEBUF, 1088);
    w32(k1, S_RDMA(3) + S_LINEBUF, 1088);
}

static void k1_frame(void)
{
    w32(k1, S_CTL2_CFG_RDY, 0);
    w32(k1, S_INT_STATUS, r32(k1, S_INT_STATUS) | S_INT_VSYNC);
}

static void test_saturn(void)
{
    struct dp_out o = { .regs = nullptr };
    uint32_t a, c;
    uint64_t seq;

    k1_firmware();
    o.regs = k1;
    o.regs_size = 0x2a000;
    o.fb = (volatile uint8_t *)fb_words;
    o.fb_size = sizeof fb_words;
    o.fb_cells[0] = o.fb_cells[1] = o.fb_cells[2] = 64;
    latch_reg = k1 + S_CTL2_CFG_RDY;
    latching = true;
    CHECK(dp_start(&o, &dp_saturn, false));
    latching = false;
    CHECK(o.width == 1920 && o.height == 1080 && o.nplanes == 3);
    /* U-Boot's layer moved to the bottom, all of it; the line buffer split. */
    CHECK(r32(k1, S_SLOT(7) + S_SLOT_EN) == (1 | 3 << 1) && r32(k1, S_SLOT(0) + S_SLOT_EN) == 0);
    CHECK(r32(k1, S_SLOT(7) + S_SLOT_TOP_RIGHT) == 1919u << 16 && r32(k1, S_SLOT(7) + S_SLOT_BOTTOM) == 1079);
    CHECK(r32(k1, S_RDMA(2) + S_LINEBUF) == (1u << 28 | 240));
    CHECK(r32(k1, S_RDMA(3) + S_LINEBUF) == (240u << 16 | (S_BANK1 - 240)));
    CHECK(r32(k1, S_INT_ENABLE) & S_INT_VSYNC);
    CHECK(r32(k1, S_CTL2_RCH_EN) == (1u << 3 | 1u << 18));

    a = alloc(&o, 0, 300, 200, DISPLAY_XRGB8888);
    c = alloc(&o, 0, 64, 64, DISPLAY_ARGB8888);
    CHECK((seq = commit(&o, 0, 1, a, 100, 50)) == 1);
    CHECK(r32(k1, S_CTL2_CFG_RDY) == 1);
    CHECK(r32(k1, S_SLOT(6) + S_SLOT_EN) == (1 | 2 << 1));
    CHECK(r32(k1, S_SLOT(6) + S_SLOT_LEFT) == 100u << 8);
    CHECK(r32(k1, S_SLOT(6) + S_SLOT_TOP_RIGHT) == (50 | 399u << 16));
    CHECK(r32(k1, S_SLOT(6) + S_SLOT_BOTTOM) == 249);
    CHECK(r32(k1, S_RDMA(2) + S_Y_OFST) == 50);
    CHECK(r32(k1, S_RDMA(2) + S_ROT_MODE) == S_FMT_XRGB8888);
    CHECK(r32(k1, S_RDMA(2) + S_CROP_END) == (299 | 199u << 16));
    CHECK(r32(k1, S_CTL2_RCH_EN) == (1u << 3 | 1u << 18 | 1u << 2));
    dp_tick(&o);
    CHECK(o.sync->ds_Disp == 0);
    k1_frame();
    dp_tick(&o);
    CHECK(o.sync->ds_Disp == seq);

    /* The cursor past the bottom right: cropped, not wrapped. */
    CHECK((seq = commit(&o, 0, 2, c, 1900, 1050)) == 2);
    CHECK(r32(k1, S_SLOT(0) + S_SLOT_EN) == (1 | 0 << 1));
    CHECK(r32(k1, S_SLOT(0) + S_SLOT_TOP_RIGHT) == (1050 | 1919u << 16));
    CHECK((r32(k1, S_SLOT(0) + S_SLOT_BOTTOM) & 0xffff) == 1079);
    CHECK(r32(k1, S_SLOT(0) + S_SLOT_BOTTOM) & S_PIXEL_ALPHA);
    CHECK(r32(k1, S_RDMA(0) + S_CROP_END) == (19 | 29u << 16));
    k1_frame();
    dp_tick(&o);

    /* hwdb's stop: the overlay and cursor off, the console's left. */
    latching = true;
    CHECK(saturn_quiet(k1, 100000000));
    latching = false;
    CHECK(r32(k1, S_SLOT(6) + S_SLOT_EN) == 0 && r32(k1, S_SLOT(0) + S_SLOT_EN) == 0);
    CHECK(r32(k1, S_SLOT(7) + S_SLOT_EN) == (1 | 3 << 1));
    CHECK(r32(k1, S_CTL2_RCH_EN) == (1u << 3 | 1u << 18));

    /* A restart finds the console in slot 7 already, and goes on. */
    latching = true;
    o = (struct dp_out){ .regs = k1, .regs_size = 0x2a000, .fb = (volatile uint8_t *)fb_words,
                         .fb_size = sizeof fb_words, .fb_cells = { 64, 64, 64, 0 } };
    CHECK(dp_saturn.take(&o, true));
    latching = false;
    CHECK(r32(k1, S_SLOT(7) + S_SLOT_EN) == (1 | 3 << 1) && r32(k1, S_SLOT(0) + S_SLOT_EN) == 0);

    /* The cmdlist on: it would load over us. */
    w32(k1, S_CMDLIST_EN + 4, 1);
    CHECK(!dp_saturn.take(&o, false));
}

/* --- fb.c: the same, composed in memory. --- */

static uint32_t read_crc(struct dp_out *o, int32_t x, int32_t y, uint32_t w, uint32_t h)
{
    memset(&msg, 0, sizeof msg);
    msg.dm_Set[0] = (struct DisplayPlane){ .dp_X = x, .dp_Y = y };
    msg.dm_Width = w;
    msg.dm_Height = h;
    return ask(o, 0, DISPLAY_READ) == 0 ? msg.dm_Crc : 0;
}

/* n pixels of one colour, summed as DISPLAY_READ sums them. */
static uint32_t crc_of(uint32_t px, unsigned n)
{
    uint8_t b[4] = { (uint8_t)px, (uint8_t)(px >> 8), (uint8_t)(px >> 16), 0 };
    uint32_t crc = 0;

    while (n--)
        crc = dp_crc32(crc, b, 4);
    return crc;
}

static void test_fb(void)
{
    static uint32_t screen[160 * 100];
    struct dp_out o = { .fb = (volatile uint8_t *)screen, .fb_size = sizeof screen,
                        .fb_cells = { 160, 100, 160, 1 } };
    uint32_t a, c;
    volatile uint32_t *pa, *pc;
    uint64_t seq;

    for (unsigned i = 0; i < 160 * 100; i++)
        screen[i] = 0x111111;
    now_ns = 0;
    CHECK(dp_start(&o, &dp_fb, false));
    CHECK(o.con_fb[0] == 0x111111);                 /* the copy starts as the screen */
    a = alloc(&o, 0, 20, 10, DISPLAY_XRGB8888);
    c = alloc(&o, 0, 4, 4, DISPLAY_ARGB8888);
    pa = (volatile uint32_t *)o.buf[a - 1].addr;
    pc = (volatile uint32_t *)o.buf[c - 1].addr;
    for (unsigned i = 0; i < o.buf[a - 1].stride / 4 * 10; i++)
        pa[i] = 0x00ff0000;                         /* red, alpha ignored */
    for (unsigned i = 0; i < o.buf[c - 1].stride / 4 * 4; i++)
        pc[i] = 0x800000ff;                         /* blue, half */
    CHECK((seq = commit(&o, 0, 1, a, 5, 5)) == 1);
    CHECK(screen[5 * 160 + 5] == 0x111111);         /* not before its frame */
    now_ns += 17000000;
    dp_tick(&o);
    CHECK(o.sync->ds_Disp == seq && screen[5 * 160 + 5] == 0xff0000 && screen[5 * 160 + 4] == 0x111111);
    CHECK(commit(&o, 0, 2, c, 6, 6) == 2);
    now_ns += 17000000;
    dp_tick(&o);
    CHECK(screen[6 * 160 + 6] == 0x7f0080);         /* blue half over red */
    CHECK(read_crc(&o, 6, 6, 1, 1) == crc_of(0x7f0080, 1));
    CHECK(read_crc(&o, 0, 0, 4, 2) == crc_of(0x111111, 8));
    CHECK(read_crc(&o, 0, 0, 160, 100) != 0 && msg.dm_Msg.mn_Error == 0);
    CHECK(read_crc(&o, 150, 0, 11, 1) == 0 && msg.dm_Msg.mn_Error == ERR_BADARG);
    CHECK(commit(&o, 0, 2, 0, 0, 0) == 3);
    now_ns += 17000000;
    dp_tick(&o);
    CHECK(screen[6 * 160 + 6] == 0xff0000);         /* the cursor gone, what's under it back */
    dp_gone(&o, 0);
    now_ns += 17000000;
    dp_tick(&o);
    CHECK(screen[5 * 160 + 5] == 0x111111 && dp_live_buffers(&o) == 0);
}

int main(void)
{
    pthread_t t;

    cix = aligned_alloc(4096, 0x20000);
    k1 = aligned_alloc(4096, 0x2a000);
    pthread_create(&t, nullptr, hardware, nullptr);
    test_komeda();
    test_saturn();
    test_fb();
    stop_thread = true;
    pthread_join(t, nullptr);
    printf("display-test: %u of %u checks failed\n", failures, checks);
    return failures != 0;
}
