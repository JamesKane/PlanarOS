/*
 * drv/display/display.h -- the display server's parts (v5-plan step 3)
 *
 * core.c keeps an output's planes, buffers, clients and commits; a
 * backend drives the controller: komeda.c the CIX's DPU, saturn.c the
 * K1's, and fb.c the firmware's framebuffer alone (QEMU's, and any
 * machine whose controller display doesn't know), which composes the
 * planes itself. console.c draws the text console on plane 0. main.c is
 * the driver; test.c runs the core and the backends on the host.
 */
#ifndef DRV_DISPLAY_DISPLAY_H
#define DRV_DISPLAY_DISPLAY_H

#include <stdint.h>

#include <planar/devices/display.h>

constexpr unsigned DP_BUFFERS = 32;
constexpr unsigned DP_CLIENTS = 8;          /* drv's DRV_CLIENTS */
constexpr uint32_t DP_MAX_SIZE = 4096;      /* a buffer's width or height, at most */

/* A plane as the controller is to show it, clipped to the screen: the
 * rectangle (sx, sy, w, h) of the buffer at 'base', at (x, y). */
struct dp_hw {
    bool on;
    uint32_t format;
    uint64_t base;
    uint32_t stride;                /* bytes */
    uint32_t sx, sy, w, h;
    uint32_t x, y;
};

struct dp_out;

struct dp_backend {
    const char *name;
    /* Take over the mode the firmware left: the output's size, its
     * planes, and the console's memory (plane 0's); every other plane
     * off. False if the controller isn't lit. 'again' if an earlier
     * display left it (DRVF_AGAIN). */
    bool (*take)(struct dp_out *o, bool again);
    /* Planes 1 to nplanes-1 as they're to be from the next latch: write
     * them and arm it. Plane 0 stays the console's. */
    void (*program)(struct dp_out *o, const struct dp_hw *hw);
    /* Its interrupt, or a tick: true if a vblank has passed since the
     * last call; *latched, if what program armed is now on the screen. */
    bool (*vblank)(struct dp_out *o, bool *latched);
    /* The console's rows y0 to y1 were drawn (fb.c composes them). */
    void (*damage)(struct dp_out *o, uint32_t y0, uint32_t y1);
    /* When it next wants dp_tick without an interrupt (fb.c's frames,
     * as CurrentTime goes); 0: never. */
    uint64_t (*deadline)(struct dp_out *o);
    bool dma;                       /* its buffers are DMA Areas (the controller reads them) */
};

extern const struct dp_backend dp_komeda, dp_saturn, dp_fb;

struct dp_buf {
    bool used, dying;
    uint8_t client;
    Handle area, task;
    uint64_t addr, size;
    uint32_t width, height, stride, format;
};

struct dp_out {
    const struct dp_backend *be;
    volatile uint8_t *regs;         /* the controller's */
    uint64_t regs_size;
    volatile uint8_t *fb;           /* the firmware's framebuffer */
    uint64_t fb_size;
    uint32_t fb_cells[4];           /* its width, height, pixels a line, format */
    uint32_t width, height, refresh_mhz;
    bool measure;                   /* the backend didn't say: refresh_mhz from the vblanks */
    unsigned nplanes;               /* the console's included */
    struct DisplayPlaneInfo plane[DISPLAY_PLANES];

    /* The console: plane 0's memory, which the console draws in. */
    volatile uint32_t *con_fb;
    uint32_t con_width, con_height, con_stride;     /* pixels */
    bool con_bgr;                   /* blue in the low byte, as 0xRRGGBB is held */

    /* Commits. want is what clients asked for, armed what the controller
     * takes at the next latch (commit armed_seq), shown what it shows. */
    struct DisplayPlane want[DISPLAY_PLANES];
    uint8_t owner[DISPLAY_PLANES];  /* client + 1; 0: nobody's */
    uint32_t armed_buf[DISPLAY_PLANES], shown_buf[DISPLAY_PLANES];
    bool armed, dirty;
    uint64_t seq, armed_seq;

    struct dp_buf buf[DP_BUFFERS];
    struct DisplaySync *sync;
    Handle sync_area;

    uint64_t be_words[32];          /* the backend's */
};

/* The services core.c needs (main.c's; test.c's on the host). */
int64_t dp_alloc(struct dp_out *o, uint64_t size, bool dma, Handle *area, uint64_t *addr);
void dp_free(Handle area);
int64_t dp_share(Handle area, Handle task, bool writable);
void dp_unshare(Handle area, Handle task);
void dp_wake(volatile uint64_t *word);
uint64_t dp_now(void);              /* nanoseconds */
void dp_say(const char *text);
void dp_clean(const volatile void *p, uint64_t n);

/* core.c */
bool dp_start(struct dp_out *o, const struct dp_backend *be, bool again);
void dp_message(struct dp_out *o, unsigned client, struct DisplayMsg *m);
void dp_gone(struct dp_out *o, unsigned client);
void dp_tick(struct dp_out *o);     /* an interrupt, or the deadline */
unsigned dp_live_buffers(const struct dp_out *o);
bool dp_streq(const char *a, const char *b);
unsigned dp_dec(char *at, uint32_t v);  /* v in decimal at 'at': the digits written */
unsigned dp_hex(char *at, uint64_t v);  /* "0x..." */

/* console.c: the text console on plane 0. */
void con_start(struct dp_out *o, bool again);
void con_write(struct dp_out *o, const uint8_t *text, uint32_t len);
bool con_fits(const struct dp_out *o);  /* 20 columns and 4 rows, at least */

#endif /* DRV_DISPLAY_DISPLAY_H */
