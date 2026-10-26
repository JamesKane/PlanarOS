/*
 * drv/display/core.c -- an output's planes, buffers and commits
 *
 * A commit changes 'want', and the output is dirty until the controller
 * has been given it: at once if nothing is armed, else at the latch of
 * what is. Commits coming between two latches go together, and ds_Disp
 * moves to the last of them. A buffer that's freed, or whose client went,
 * dies: it's freed once neither the armed nor the shown planes show it.
 * The controllers latch at the start of a frame and read nothing of the
 * last after it, so ds_Safe moves with ds_Disp.
 */
#include <planar/syscalls.h>

#include "display.h"

static struct dp_buf *buffer(struct dp_out *o, unsigned client, uint32_t id)
{
    struct dp_buf *b;

    if (id == 0 || id > DP_BUFFERS)
        return nullptr;
    b = &o->buf[id - 1];
    return b->used && !b->dying && b->client == client ? b : nullptr;
}

static bool shows(const struct dp_out *o, uint32_t id)
{
    for (unsigned p = 1; p < o->nplanes; p++)
        if (o->shown_buf[p] == id || (o->armed && o->armed_buf[p] == id))
            return true;
    return false;
}

/* The dead buffers nothing shows any more. */
static void sweep(struct dp_out *o)
{
    for (unsigned i = 0; i < DP_BUFFERS; i++) {
        struct dp_buf *b = &o->buf[i];

        if (b->used && b->dying && !shows(o, i + 1)) {
            if (b->task)
                dp_unshare(b->area, b->task);
            dp_free(b->area);
            *b = (struct dp_buf){ 0 };
        }
    }
}

/* Plane p's want, clipped to the screen. */
static struct dp_hw clip(const struct dp_out *o, unsigned p)
{
    const struct DisplayPlane *w = &o->want[p];
    const struct dp_buf *b;
    int64_t x0, y0, x1, y1;

    if (!w->dp_Buffer)
        return (struct dp_hw){ 0 };
    b = &o->buf[w->dp_Buffer - 1];
    x0 = w->dp_X < 0 ? 0 : w->dp_X;
    y0 = w->dp_Y < 0 ? 0 : w->dp_Y;
    x1 = (int64_t)w->dp_X + b->width;
    y1 = (int64_t)w->dp_Y + b->height;
    if (x1 > o->width)
        x1 = o->width;
    if (y1 > o->height)
        y1 = o->height;
    if (x1 <= x0 || y1 <= y0)
        return (struct dp_hw){ 0 };
    return (struct dp_hw){
        .on = true, .format = b->format, .base = b->addr, .stride = b->stride,
        .sx = (uint32_t)(x0 - w->dp_X), .sy = (uint32_t)(y0 - w->dp_Y),
        .w = (uint32_t)(x1 - x0), .h = (uint32_t)(y1 - y0), .x = (uint32_t)x0, .y = (uint32_t)y0,
    };
}

static void arm(struct dp_out *o)
{
    struct dp_hw hw[DISPLAY_PLANES] = { 0 };

    if (!o->dirty || o->armed)
        return;
    for (unsigned p = 1; p < o->nplanes; p++) {
        hw[p] = clip(o, p);
        o->armed_buf[p] = hw[p].on ? o->want[p].dp_Buffer : 0;
    }
    o->be->program(o, hw);
    o->armed = true;
    o->armed_seq = o->seq;
    o->dirty = false;
}

bool dp_start(struct dp_out *o, const struct dp_backend *be, bool again)
{
    uint64_t page;

    o->be = be;
    o->refresh_mhz = 0;
    if (!be->take(o, again) || dp_alloc(o, 4096, false, false, &o->sync_area, &page) != 0)
        return false;
    o->sync = (struct DisplaySync *)page;
    o->measure = o->refresh_mhz == 0;
    return true;
}

static int64_t info(struct dp_out *o, struct DisplayMsg *m)
{
    int64_t e = dp_share(o->sync_area, m->dm_Task, false);

    if (e != 0 && e != ERR_EXISTS)
        return e;
    m->dm_Width = o->width;
    m->dm_Height = o->height;
    m->dm_RefreshMilliHz = o->refresh_mhz;
    m->dm_Sync = (uint64_t)o->sync;
    m->dm_Count = o->nplanes;
    m->dm_Buffers = dp_live_buffers(o);
    m->dm_Flags = o->be->cached ? DISPLAYI_CACHED : 0;
    for (unsigned p = 0; p < DISPLAY_PLANES; p++)
        m->dm_Plane[p] = p < o->nplanes ? o->plane[p] : (struct DisplayPlaneInfo){ 0 };
    return 0;
}

static int64_t alloc(struct dp_out *o, unsigned client, struct DisplayMsg *m)
{
    struct dp_buf *b = nullptr;
    uint32_t stride;
    uint64_t size, addr;
    Handle area;
    int64_t e;

    if (m->dm_Width == 0 || m->dm_Height == 0 || m->dm_Width > DP_MAX_SIZE ||
        m->dm_Height > DP_MAX_SIZE ||
        (m->dm_Format != DISPLAY_XRGB8888 && m->dm_Format != DISPLAY_ARGB8888) ||
        m->dm_Flags & ~DISPLAYA_CACHED)
        return ERR_BADARG;
    for (unsigned i = 0; i < DP_BUFFERS && !b; i++)
        if (!o->buf[i].used)
            b = &o->buf[i];
    if (!b)
        return ERR_FULL;
    stride = (m->dm_Width * 4 + 63) & ~63u;     /* both controllers' lines start 64-aligned */
    size = ((uint64_t)stride * m->dm_Height + 4095) & ~4095ull;
    if ((e = dp_alloc(o, size, o->be->dma, m->dm_Flags & DISPLAYA_CACHED, &area, &addr)) != 0)
        return e;
    if ((e = dp_share(area, m->dm_Task, true)) != 0) {
        dp_free(area);
        return e;
    }
    *b = (struct dp_buf){
        .used = true, .client = (uint8_t)client, .area = area, .task = m->dm_Task, .addr = addr,
        .size = size, .width = m->dm_Width, .height = m->dm_Height, .stride = stride,
        .format = m->dm_Format,
    };
    m->dm_Buffer = (uint32_t)(b - o->buf) + 1;
    m->dm_Address = addr;
    m->dm_Stride = stride;
    return 0;
}

static int64_t free_buf(struct dp_out *o, unsigned client, uint32_t id)
{
    struct dp_buf *b = buffer(o, client, id);

    if (!b)
        return ERR_NOTFOUND;
    for (unsigned p = 1; p < o->nplanes; p++)
        if (o->want[p].dp_Buffer == id)
            return ERR_BUSY;        /* commit its plane off first */
    b->dying = true;
    sweep(o);
    return 0;
}

static int64_t commit(struct dp_out *o, unsigned client, struct DisplayMsg *m)
{
    if (m->dm_Count == 0 || m->dm_Count > DISPLAY_PLANES)
        return ERR_BADARG;
    /* All of it, or none. */
    for (unsigned k = 0; k < m->dm_Count; k++) {
        const struct DisplayPlane *s = &m->dm_Set[k];
        const struct dp_buf *b;

        if (s->dp_Plane == 0 || s->dp_Plane >= o->nplanes)
            return s->dp_Plane == 0 ? ERR_RIGHTS : ERR_BADARG;     /* 0 is the console's */
        if (o->owner[s->dp_Plane] && o->owner[s->dp_Plane] != client + 1)
            return ERR_BUSY;
        for (unsigned j = 0; j < k; j++)
            if (m->dm_Set[j].dp_Plane == s->dp_Plane)
                return ERR_BADARG;
        if (!s->dp_Buffer)
            continue;
        if (!(b = buffer(o, client, s->dp_Buffer)))
            return ERR_NOTFOUND;
        if (!(o->plane[s->dp_Plane].di_Formats & 1u << b->format) ||
            b->width > o->plane[s->dp_Plane].di_MaxWidth ||
            b->height > o->plane[s->dp_Plane].di_MaxHeight)
            return ERR_BADARG;
    }
    for (unsigned k = 0; k < m->dm_Count; k++) {
        const struct DisplayPlane *s = &m->dm_Set[k];

        o->want[s->dp_Plane] = *s;
        o->owner[s->dp_Plane] = s->dp_Buffer ? (uint8_t)(client + 1) : 0;
    }
    m->dm_Seq = ++o->seq;
    o->dirty = true;
    arm(o);
    return 0;
}

uint32_t dp_crc32(uint32_t crc, const uint8_t *p, uint64_t n)
{
    static uint32_t table[256];

    if (!table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t v = i;

            for (unsigned k = 0; k < 8; k++)
                v = v >> 1 ^ (0xedb88320u & -(v & 1));
            table[i] = v;
        }
    crc = ~crc;
    while (n--)
        crc = crc >> 8 ^ table[(crc ^ *p++) & 0xff];
    return ~crc;
}

/* A rectangle of the screen's CRC-32, a row at a time. */
static int64_t read_screen(const struct dp_out *o, struct DisplayMsg *m)
{
    int64_t x = m->dm_Set[0].dp_X, y = m->dm_Set[0].dp_Y;
    uint8_t row[256];
    uint32_t crc = 0;

    if (!o->be->pixel)
        return ERR_NOSYS;
    if (x < 0 || y < 0 || !m->dm_Width || !m->dm_Height || x + m->dm_Width > o->width ||
        y + m->dm_Height > o->height)
        return ERR_BADARG;
    for (uint32_t j = 0; j < m->dm_Height; j++)
        for (uint32_t i = 0; i < m->dm_Width;) {
            unsigned n = 0;

            for (; n < sizeof row && i < m->dm_Width; i++, n += 4) {
                uint32_t px = o->be->pixel(o, (uint32_t)x + i, (uint32_t)y + j);

                row[n] = (uint8_t)px;
                row[n + 1] = (uint8_t)(px >> 8);
                row[n + 2] = (uint8_t)(px >> 16);
                row[n + 3] = 0;
            }
            crc = dp_crc32(crc, row, n);
        }
    m->dm_Crc = crc;
    return 0;
}

void dp_message(struct dp_out *o, unsigned client, struct DisplayMsg *m)
{
    int64_t e;

    switch (m->dm_Op) {
    case DISPLAY_INFO:
        e = info(o, m);
        break;
    case DISPLAY_ALLOC:
        e = alloc(o, client, m);
        break;
    case DISPLAY_FREE:
        e = free_buf(o, client, m->dm_Buffer);
        break;
    case DISPLAY_COMMIT:
        e = commit(o, client, m);
        break;
    case DISPLAY_READ:
        e = read_screen(o, m);
        break;
    default:
        e = ERR_BADARG;
    }
    m->dm_Msg.mn_Error = (int32_t)e;
}

/* Its planes off at the next latch; its buffers freed after it. Its
 * mappings went with it if it died; if it only closed, it keeps them
 * until they're freed, and unshared. */
void dp_gone(struct dp_out *o, unsigned client)
{
    for (unsigned p = 1; p < o->nplanes; p++)
        if (o->owner[p] == client + 1) {
            o->owner[p] = 0;
            o->want[p] = (struct DisplayPlane){ .dp_Plane = p };
            o->dirty = true;
        }
    for (unsigned i = 0; i < DP_BUFFERS; i++)
        if (o->buf[i].used && o->buf[i].client == client)
            o->buf[i].dying = true;
    arm(o);
    sweep(o);
}

void dp_tick(struct dp_out *o)
{
    bool latched = false;

    uint64_t now;

    if (!o->be->vblank(o, &latched))
        return;
    now = dp_now();
    /* A controller that doesn't say its rate: as the vblanks come. */
    if (o->measure && o->sync->ds_Time && now > o->sync->ds_Time)
        o->refresh_mhz = (uint32_t)(1000000000000ull / (now - o->sync->ds_Time));
    o->sync->ds_Time = now;
    o->sync->ds_Vblank++;
    if (latched && o->armed) {
        for (unsigned p = 1; p < o->nplanes; p++)
            o->shown_buf[p] = o->armed_buf[p];
        o->armed = false;
        o->sync->ds_Disp = o->armed_seq;
        o->sync->ds_Safe = o->armed_seq;
        dp_wake(&o->sync->ds_Disp);
        dp_wake(&o->sync->ds_Safe);
        sweep(o);
        arm(o);
    }
    dp_wake(&o->sync->ds_Vblank);
}

unsigned dp_live_buffers(const struct dp_out *o)
{
    unsigned n = 0;

    for (unsigned i = 0; i < DP_BUFFERS; i++)
        n += o->buf[i].used;
    return n;
}

bool dp_streq(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return *a == *b;
}

unsigned dp_dec(char *at, uint32_t v)
{
    char dg[12];
    unsigned k = 0, n = 0;

    do
        dg[k++] = (char)('0' + v % 10);
    while ((v /= 10) && k < sizeof dg);
    while (k)
        at[n++] = dg[--k];
    return n;
}

unsigned dp_hex(char *at, uint64_t v)
{
    unsigned n = 0, k = 60;

    at[n++] = '0';
    at[n++] = 'x';
    while (k && !(v >> k & 0xf))
        k -= 4;
    for (;; k -= 4) {
        at[n++] = "0123456789abcdef"[v >> k & 0xf];
        if (!k)
            break;
    }
    return n;
}
