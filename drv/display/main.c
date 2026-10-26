/*
 * drv/display/main.c -- display: the screen the firmware left lit
 * (v4-plan step 7; v5-plan step 3)
 *
 * hwdb leases display the framebuffer the firmware left (UEFI's GOP, or
 * U-Boot's), its geometry in the node's cells, and with it, where hwdb
 * knows the controller scanning it out (HW_Q_SCANOUT), that controller:
 * the CIX's DPU4 (komeda.c), the K1's HDMI DPU (saturn.c). With one,
 * display takes over the mode the firmware set; without, or if it isn't
 * lit, fb.c composes the planes itself. core.c has the planes, console.c
 * the text console on plane 0.
 *
 * It serves "con.0", the console class (DRV_WRITE of up to 64 bytes of
 * text), and "display.0" (planar/devices/display.h), both on one
 * connection's messages: their ops don't overlap.
 */
#include <stddef.h>

#include <planar/drv.h>
#include <planar/syscalls.h>

#include "display.h"

static struct drv *drv;
static struct dp_out out;
static struct drv_dev *fb_dev, *ctl_dev;
static bool started;

/* A controller reads its planes past the caches, so they're NC unless
 * the client asked for them cached (and cleans what it writes); fb.c's
 * are read by the CPU. */
int64_t dp_alloc(struct dp_out *o, uint64_t size, bool dma, bool cached, Handle *area, uint64_t *addr)
{
    void *at;
    int64_t h = dma ? AllocDmaArea(drv_lease(drv), size, o->be == &dp_fb || cached ? 0 : AREAF_NC, &at)
                    : AllocArea(size, AREAF_CLEAR, &at);

    if (h < 0)
        return h;
    *area = (Handle)h;
    *addr = (uint64_t)at;
    return 0;
}

void dp_free(Handle area)
{
    FreeArea(area);
}

int64_t dp_share(Handle area, Handle task, bool writable)
{
    return ShareArea(area, task, AREAR_READ | (writable ? AREAR_WRITE : 0));
}

void dp_unshare(Handle area, Handle task)
{
    UnshareArea(area, task);
}

void dp_wake(volatile uint64_t *word)
{
    WakeAddr(word, UINT32_MAX);
}

uint64_t dp_now(void)
{
    return CurrentTime();
}

void dp_say(const char *text)
{
    drv_log(drv, nullptr, text);
}

void dp_clean(const volatile void *p, uint64_t n)
{
    drv_dma_clean((const void *)p, n);
}

static int bind(struct drv *d, struct drv_dev *dev)
{
    const struct DrvDevice *info = dev->info;

    drv = d;
    if (dp_streq(info->dd_Key, "simple-framebuffer")) {
        if (fb_dev || !info->dd_Cells[0] || !info->dd_Cells[1] ||
            info->dd_Cells[2] < info->dd_Cells[0] || info->dd_Cells[3] > 1 ||
            (uint64_t)info->dd_Cells[2] * info->dd_Cells[1] * 4 > info->dd_Size)
            return DRV_DECLINE;
        fb_dev = dev;
        return DRV_OK;
    }
    if (ctl_dev || !info->dd_Size)
        return DRV_DECLINE;
    ctl_dev = dev;
    return DRV_OK;
}

static const struct dp_backend *native(const char *key)
{
    if (dp_streq(key, "CIXH5010"))
        return &dp_komeda;
    if (dp_streq(key, "ky,dpu"))
        return &dp_saturn;
    return nullptr;
}

static void say_mode(const char *how)
{
    char line[96];
    unsigned n = 0;

    for (const char *s = how; *s && n < 40; s++)
        line[n++] = *s;
    line[n++] = ' ';
    n += dp_dec(line + n, out.width);
    line[n++] = 'x';
    n += dp_dec(line + n, out.height);
    for (const char *s = ", planes "; *s; s++)
        line[n++] = *s;
    n += dp_dec(line + n, out.nplanes);
    line[n] = '\0';
    dp_say(line);
}

/* Once every device is in: the backend, the console, the two ports. */
static int start(struct drv *d, struct drv_dev *dev)
{
    bool again = d->info.hm_Flags & DRVF_AGAIN;
    const struct dp_backend *be = ctl_dev ? native(ctl_dev->info->dd_Key) : nullptr;
    int64_t e;

    if (started)
        return 0;
    started = true;
    if (!fb_dev) {
        drv_log(d, dev, "no framebuffer to draw the console on");
        return -1;
    }
    out.fb = fb_dev->regs;
    out.fb_size = fb_dev->info->dd_Size;
    memcpy(out.fb_cells, fb_dev->info->dd_Cells, sizeof out.fb_cells);
    if (be) {
        out.regs = ctl_dev->regs;
        out.regs_size = ctl_dev->info->dd_Size;
        if (!dp_start(&out, be, again)) {
            dp_say("the controller isn't lit: the framebuffer alone");
            out = (struct dp_out){ .fb = out.fb, .fb_size = out.fb_size };
            memcpy(out.fb_cells, fb_dev->info->dd_Cells, sizeof out.fb_cells);
            be = nullptr;
        }
    }
    if (!be && !dp_start(&out, be = &dp_fb, again))
        return -1;
    if (!con_fits(&out))
        return -1;
    con_start(&out, again);
    say_mode(be == &dp_fb ? "the framebuffer," : be == &dp_komeda ? "DPU (komeda)," : "DPU (saturn),");
    if ((e = drv_publish_as(d, fb_dev, "con.0")) != 0)
        return (int)e;
    return (int)drv_publish_as(d, fb_dev, "display.0");
}

static void message(struct drv *d, struct drv_dev *, unsigned c, struct Message *m)
{
    uint32_t op = ((struct DrvRequest *)m)->dr_Op;

    if (op == DRV_WRITE) {
        struct DrvRequest *r = (struct DrvRequest *)m;
        uint32_t len = r->dr_Len;

        if (len > sizeof r->dr_Data) {
            m->mn_Error = (int32_t)ERR_BADARG;
        } else {
            uint8_t text[sizeof r->dr_Data];

            memcpy(text, r->dr_Data, len);      /* the client's memory: read once */
            con_write(&out, text, len);
            m->mn_Error = 0;
        }
    } else if (op >= DISPLAY_INFO && drv_in_heap(d, c, (uint64_t)m, sizeof(struct DisplayMsg))) {
        struct DisplayMsg copy;

        memcpy(&copy, m, sizeof copy);
        dp_message(&out, c, &copy);
        memcpy(m, &copy, sizeof copy);
    } else {
        m->mn_Error = (int32_t)ERR_BADARG;
    }
    drv_reply(d, c, m);
}

static void gone(struct drv *, struct drv_dev *, unsigned c)
{
    dp_gone(&out, c);
}

static void interrupt(struct drv *, struct drv_dev *)
{
    dp_tick(&out);
}

/* fb.c's frames: a tick at each deadline. */
static bool poll(struct drv *)
{
    if (out.be && out.be->deadline)
        dp_tick(&out);
    return false;
}

static uint64_t deadline(struct drv *)
{
    return out.be && out.be->deadline ? out.be->deadline(&out) : 0;
}

static const struct drv_ops ops = {
    .name = "display", .bind = bind, .start = start, .message = message,
    .message_size = sizeof(struct DrvRequest), .gone = gone, .interrupt = interrupt,
    .poll = poll, .deadline = deadline,
};

int main(uint64_t token)
{
    drv_main(token, &ops);
}
