/*
 * servers/conwin/test.c -- a console window's text on the host (make
 * conwin-test)
 *
 * term.c alone, with the sanitizers: what's written (wraps, control
 * codes, CSI, UTF-8 and its mistakes), the scrollback and the view, a
 * resize that reflows nothing, and typing (editing, the queue, the end
 * of the input). Then drawing, in System Mono from lib/font linked in,
 * through lib/gfx's scalar entries and NEON's on an Arm host: the window
 * test's Shell (conformance/shellwin.h), drawn a little at a time at
 * random and then whole, which must agree, and give the CRCs window.c
 * reads back on QEMU. The pictures are in build/conwin.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <planar/font.h>

#include "../../conformance/shellwin.h"
#include "../../lib/gfx/rows.h"
#include "conwin.h"
#include "term.h"

extern const struct FontIFace *gfx_host_ifont;

/* Areas: the host's memory, page aligned and zeroed (the fonts' caches). */
static void *areas[16];

int64_t AllocArea(uint64_t size, uint32_t, void **address)
{
    size = (size + 4095) & ~4095ull;
    for (unsigned i = 0; i < sizeof areas / sizeof areas[0]; i++)
        if (!areas[i]) {
            if (!(areas[i] = aligned_alloc(4096, size)))
                return ERR_NOMEM;
            memset(areas[i], 0, size);
            *address = areas[i];
            return i + 1;
        }
    return ERR_NOMEM;
}

int64_t FreeArea(Handle h)
{
    if (h < 1 || h > sizeof areas / sizeof areas[0] || !areas[h - 1])
        return ERR_BADHANDLE;
    free(areas[h - 1]);
    areas[h - 1] = nullptr;
    return 0;
}

int64_t QueryArea(const void *, struct AreaInfo *)
{
    return ERR_NOTFOUND;
}

#define TABLE(fill, blt, text, path, mask, fpath, grad, run) { \
    .AllocBitMap = AllocBitMap, .FreeBitMap = FreeBitMap, .InitBitMap = InitBitMap, \
    .InitRastPort = InitRastPort, .RectFill = fill, .BltBitMap = blt, .DrawBevel = DrawBevel, \
    .Text = text, .PathBounds = PathBounds, .PathMaskScratch = PathMaskScratch, .PathMask = path, \
    .BltMask = mask, .FillPath = fpath, .FillGradient = grad, .PathRoundRect = PathRoundRect, \
    .InitTmpRas = InitTmpRas, .OpenTextFont = OpenTextFont, .CloseTextFont = CloseTextFont, \
    .SetFont = SetFont, .TextLength = TextLength, .TextRun = run }

static const struct GfxIFace scalar =
    TABLE(RectFill, BltBitMap, Text, PathMask, BltMask, FillPath, FillGradient, TextRun);

#ifdef GFX_TEST_NEON
static const struct GfxIFace neon = TABLE(RectFillVector, BltBitMapVector, TextVector, PathMaskVector,
                                          BltMaskVector, FillPathVector, FillGradientVector, TextRunVector);
#endif

static struct term_cell cells[TERM_LINES * TERM_COLS];
static struct term t;
static unsigned failures;

#define CHECK(cond) \
    do \
        if (!(cond)) { \
            printf("conwin-test: FAILED: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++; \
        } \
    while (0)

static void put(const char *s)
{
    term_write(&t, s, (uint32_t)strlen(s));
}

static void type(const char *s)
{
    while (*s)
        term_key(&t, (uint8_t)*s++);
}

/* Row r of the window, as text (ASCII; others '?', blanks ' '), trailing blanks cut. */
static const char *row(uint32_t r)
{
    static char text[TERM_COLS + 1];
    uint32_t n = 0, end = 0;

    for (uint32_t c = 0; c < t.cols; c++) {
        uint16_t ch = term_at(&t, r, c)->ch;

        text[n++] = !ch ? ' ' : ch < 0x80 ? (char)ch : '?';
        end = ch ? n : end;
    }
    text[end] = '\0';
    return text;
}

/* --- What's written. --- */

static void writing(void)
{
    char buf[64];

    term_init(&t, cells, 10, 4);
    put("0123456789abcdefghijklmnopqrstu");
    CHECK(t.lines == 4 && t.cx == 1);                   /* wrapped as written: 10, 10, 10 and 1 */
    CHECK(!strcmp(row(0), "0123456789") && !strcmp(row(3), "u"));
    put("\n\nx");                                       /* past the window: the top goes */
    CHECK(!strcmp(row(0), "klmnopqrst") && !strcmp(row(1), "u") && !strcmp(row(3), "x"));

    term_init(&t, cells, 20, 4);
    put("hello\rj\tz\bZ");
    CHECK(!strcmp(row(0), "jello   Z"));
    put("\n\t\t\tq");                                   /* tabs stop at the edge */
    CHECK(t.cx == 20 && !strcmp(row(1), "                   q"));
    put("\x1b[32mA\x1b[0mB\x9b" "1;95mC\x1b[22;39mD\x1b[?25hE\x1b" "7F");
    CHECK(term_at(&t, 2, 0)->pen == 3 && term_at(&t, 2, 1)->pen == 0 && term_at(&t, 2, 2)->pen == 14 &&
          term_at(&t, 2, 2)->bold && !term_at(&t, 2, 3)->bold && term_at(&t, 2, 3)->pen == 0);
    CHECK(!strcmp(row(2), "ABCDEF"));                   /* unknown sequences dropped, and ESC 7's 7 */
    put("\nabcdef\x1b[3D\x1b[K");                       /* a cursor move's dropped; K clears from the cursor */
    CHECK(!strcmp(row(3), "abcdef") || !strcmp(row(3), "abc"));
    put("\r\x1b[K");
    CHECK(!strcmp(row(3), ""));

    term_init(&t, cells, 20, 4);
    put("\xc3\xa9\xe2\x94\x8c\xf0\x9f\x98\x80|\xff|\xe2\x94" "A|\xed\xa0\x80|");
    CHECK(term_at(&t, 0, 0)->ch == 0xe9 && term_at(&t, 0, 1)->ch == 0x250c && term_at(&t, 0, 2)->ch == 0xfffd);
    CHECK(term_at(&t, 0, 4)->ch == 0xfffd && term_at(&t, 0, 6)->ch == 0xfffd && term_at(&t, 0, 7)->ch == 'A');
    CHECK(term_at(&t, 0, 9)->ch == 0xfffd && term_at(&t, 0, 10)->ch == '|');

    /* A clear: the cursor's line, new, at the top; what was is scrollback. */
    term_init(&t, cells, 20, 4);
    put("one\ntwo\fthree");
    CHECK(!strcmp(row(0), "three") && !strcmp(row(1), ""));
    term_scroll(&t, 1);
    CHECK(!strcmp(row(0), "two") && !strcmp(row(1), "three"));
    term_scroll(&t, 100);
    CHECK(!strcmp(row(0), "one") && t.view == 2);
    put("\x1b[2J");
    CHECK(t.view == 0 && !strcmp(row(0), ""));

    /* Scrollback: as far back as the ring holds, and the view stays put as lines come. */
    term_init(&t, cells, 20, 5);
    for (unsigned i = 0; i < 2 * TERM_LINES; i++) {
        snprintf(buf, sizeof buf, "line %u\n", i);
        put(buf);
    }
    CHECK(!strcmp(row(3), "line 1999") && !strcmp(row(4), ""));
    term_scroll(&t, INT32_MAX);
    CHECK(t.view == TERM_LINES - 5);
    snprintf(buf, sizeof buf, "line %u", TERM_LINES + 1);
    CHECK(!strcmp(row(0), buf));                        /* the oldest kept */
    term_scroll(&t, -(int32_t)(TERM_LINES - 5) + 3);
    CHECK(!strcmp(row(0), "line 1993"));
    put("more\n");
    CHECK(!strcmp(row(0), "line 1993"));
    term_scroll(&t, -1000);
    CHECK(t.view == 0 && !strcmp(row(3), "more"));

    /* A resize reflows nothing: lines are cut at the edge. */
    term_init(&t, cells, 40, 4);
    put("abcdefghijklmnopqrstuvwxyz0123\nnext");
    term_resize(&t, 10, 4);
    CHECK(!strcmp(row(0), "abcdefghij") && !strcmp(row(1), "next") && t.lines == 2);
    term_resize(&t, 40, 4);
    CHECK(!strcmp(row(0), "abcdefghijklmnopqrstuvwxyz0123"));
    term_resize(&t, 40, 1);
    CHECK(!strcmp(row(0), "next"));
}

/* --- Typing. --- */

static void typing(void)
{
    char buf[64];
    uint32_t n;

    term_init(&t, cells, 40, 4);
    put("> ");
    type("ab\bc\x7f");
    CHECK(t.inlen == 1 && !strcmp(row(0), "> a") && t.qlen == 0);
    term_key(&t, 0x01);                                 /* a control code but those it knows: nothing */
    term_key(&t, 0xe9);                                 /* é, as two bytes */
    CHECK(t.inlen == 3 && term_at(&t, 0, 3)->ch == 0xe9);
    term_key(&t, 8);                                    /* both of them */
    CHECK(t.inlen == 1);
    type("bc\r");
    CHECK(t.qlen == 4 && t.lines == 2 && t.cx == 0);
    type("xyz\r");
    n = term_read(&t, buf, 3);
    CHECK(n == 3 && !memcmp(buf, "abc", 3));
    n = term_read(&t, buf, sizeof buf);
    CHECK(n == 5 && !memcmp(buf, "\nxyz\n", 5) && term_read(&t, buf, 1) == 0);
    type("gone\x15" "kept\x18" "k\r");
    CHECK(term_read(&t, buf, sizeof buf) == 2 && buf[0] == 'k' && !strcmp(row(2), "k"));
    term_key(&t, 0x1c);
    CHECK(t.eof && t.qlen == 0);

    /* Typing brings the view back to the bottom. */
    term_init(&t, cells, 40, 2);
    put("1\n2\n3\n");
    term_scroll(&t, 2);
    CHECK(t.view == 2);
    type("x");
    CHECK(t.view == 0 && !strcmp(row(1), "x"));

    /* A line too long to keep is kept as far as it goes. */
    term_init(&t, cells, 40, 2);
    for (unsigned i = 0; i < 2 * TERM_INPUT; i++)
        term_key(&t, 'q');
    CHECK(t.inlen == TERM_INPUT - 2);
    type("\r");
    CHECK(term_read(&t, buf, sizeof buf) == 64 && t.qlen == TERM_INPUT - 1 - 64);
}

/* --- Drawing. --- */

static uint32_t crc_of(const struct BitMap *bm)
{
    uint32_t crc = ~0u;

    for (uint32_t y = 0; y < bm->bm_Height; y++)
        for (uint32_t x = 0; x < bm->bm_Width; x++) {
            uint32_t px = ((const uint32_t *)((const uint8_t *)bm->bm_BaseAddr + (uint64_t)y * bm->bm_BytesPerRow))[x] &
                          0xffffff;

            for (unsigned k = 0; k < 32; k += 8) {
                crc ^= px >> k & 0xff;
                for (unsigned i = 0; i < 8; i++)
                    crc = crc >> 1 ^ (0xedb88320u & -(crc & 1));
            }
        }
    return ~crc;
}

static void picture(const char *name, const struct BitMap *bm)
{
    char path[128];
    FILE *f;

    snprintf(path, sizeof path, "build/conwin/%s.ppm", name);
    if (!(f = fopen(path, "wb")))
        return;
    fprintf(f, "P6\n%u %u\n255\n", bm->bm_Width, bm->bm_Height);
    for (uint32_t y = 0; y < bm->bm_Height; y++)
        for (uint32_t x = 0; x < bm->bm_Width; x++) {
            uint32_t p = ((const uint32_t *)((const uint8_t *)bm->bm_BaseAddr + (uint64_t)y * bm->bm_BytesPerRow))[x];
            uint8_t rgb[3] = { (uint8_t)(p >> 16), (uint8_t)(p >> 8), (uint8_t)p };

            fwrite(rgb, 1, 3, f);
        }
    fclose(f);
}

/* The window test's stream, as the Shells write it and conwin echoes it. */
static void shell_stream(char *out, size_t size)
{
    size_t n = 0;

    n += (size_t)snprintf(out + n, size - n, "%s", sw_typed);
    n += (size_t)snprintf(out + n, size - n, "\n");
    for (unsigned i = 0; i < sizeof sw_script / sizeof sw_script[0]; i++)
        n += (size_t)snprintf(out + n, size - n, "%s%s\n%s", sw_prompt, sw_script[i][0], sw_script[i][1]);
    snprintf(out + n, size - n, "%s", sw_prompt);
}

static uint64_t seed = 1;

static uint32_t rnd(uint32_t n)
{
    seed = seed * 6364136223846793005ull + 1442695040888963407ull;
    return (uint32_t)(seed >> 33) % n;
}

/* The window test's Shell, drawn by 'g': the inside's CRC when the
 * prompt's up, and when the script's run. Drawn as it comes, in pieces
 * at random, and again whole: the two must agree. */
static bool shell(const char *name, const struct GfxIFace *g, const struct term_font *f, uint32_t *ready,
                  uint32_t *done)
{
    static char stream[4096];
    struct BitMap *bm = g->AllocBitMap(SW_WIDTH, SW_HEIGHT, PIXFMT_XRGB8888, 0, nullptr);
    struct BitMap *whole = g->AllocBitMap(SW_WIDTH, SW_HEIGHT, PIXFMT_XRGB8888, 0, nullptr);
    struct RastPort rp, wrp;
    struct GfxRect r;
    char got[64];
    size_t len, at = 0;
    bool ok = true;

    if (!bm || !whole)
        return false;
    g->InitRastPort(&rp, bm);
    g->InitRastPort(&wrp, whole);
    term_init(&t, cells, CONWIN_COLS, CONWIN_ROWS);
    if (term_width(f, CONWIN_COLS) != SW_WIDTH || term_height(f, CONWIN_ROWS) != SW_HEIGHT) {
        printf("conwin-test: FAILED: System Mono's cells are %ux%u, not shellwin.h's\n", f->cw, f->ch);
        return false;
    }
    term_draw(g, f, &rp, SW_WIDTH, SW_HEIGHT, &t, &r);
    put(sw_banner);
    put(sw_prompt);
    term_draw(g, f, &rp, SW_WIDTH, SW_HEIGHT, &t, &r);
    *ready = crc_of(bm);
    if (!strcmp(name, "scalar"))
        picture("ready", bm);

    /* Typed (the echo's the window's own), read, then the rest written. */
    shell_stream(stream, sizeof stream);
    len = strlen(stream);
    for (const char *k = sw_typed; *k; k++) {
        term_key(&t, (uint8_t)*k);
        if (rnd(4) == 0)
            term_draw(g, f, &rp, SW_WIDTH, SW_HEIGHT, &t, &r);
    }
    term_key(&t, '\r');
    at = strlen(sw_typed) + 1;
    ok &= term_read(&t, got, sizeof got) == at && !memcmp(got, stream, at - 1) && got[at - 1] == '\n';
    while (at < len) {
        size_t n = 1 + rnd(40);

        n = n > len - at ? len - at : n;
        term_write(&t, stream + at, (uint32_t)n);
        at += n;
        if (rnd(3) == 0)
            term_draw(g, f, &rp, SW_WIDTH, SW_HEIGHT, &t, &r);
    }
    term_draw(g, f, &rp, SW_WIDTH, SW_HEIGHT, &t, &r);
    *done = crc_of(bm);
    t.all = true;
    term_draw(g, f, &wrp, SW_WIDTH, SW_HEIGHT, &t, &r);
    if (crc_of(whole) != *done) {
        printf("conwin-test: FAILED: %s: the Shell drawn in pieces isn't as it is drawn whole\n", name);
        picture("pieces", bm);
        picture("whole", whole);
        ok = false;
    }
    if (!strcmp(name, "scalar"))
        picture("shell", bm);

    /* Scrolled back a line and an inactive cursor, to look at. */
    term_scroll(&t, 1);
    term_set_active(&t, false);
    term_draw(g, f, &rp, SW_WIDTH, SW_HEIGHT, &t, &r);
    term_scroll(&t, -1);
    term_draw(g, f, &rp, SW_WIDTH, SW_HEIGHT, &t, &r);
    if (!strcmp(name, "scalar"))
        picture("inactive", bm);
    g->FreeBitMap(bm);
    g->FreeBitMap(whole);
    return ok;
}

/* cmd/scripts/window, line by line, as shellwin.h has it. */
static bool script_agrees(void)
{
    static char text[4096];
    FILE *f = fopen("cmd/scripts/window", "rb");
    size_t n;
    char *line = text;

    if (!f)
        return false;
    n = fread(text, 1, sizeof text - 1, f);
    fclose(f);
    text[n] = '\0';
    for (unsigned i = 0; i < sizeof sw_script / sizeof sw_script[0]; i++) {
        char *end = strchr(line, '\n');

        if (!end)
            return false;
        *end = '\0';
        if (strcmp(line, sw_script[i][0]) != 0) {
            printf("conwin-test: cmd/scripts/window's line %u isn't shellwin.h's\n", i + 1);
            return false;
        }
        line = end + 1;
    }
    return *line == '\0';
}

int main(void)
{
    static const struct FontIFace font = {
        .SystemFont = SystemFont, .OpenFont = OpenFont, .FontInfo = FontInfo, .FontName = FontName,
        .MapCodepoint = MapCodepoint, .GetGlyphMetrics = GetGlyphMetrics, .GlyphOutline = GlyphOutline,
        .ScaleUnits = ScaleUnits, .ShapeRun = ShapeRun, .FitLine = FitLine, .LineSpacing = LineSpacing,
    };
    struct term_font f;
    struct TextFont *regular, *bold;
    uint32_t ready, done;

    writing();
    typing();
    if (failures)
        return 1;
    printf("conwin-test: ok: written text (wraps, controls, CSI, UTF-8), the scrollback and view, a resize, "
           "and typing\n");

    gfx_host_ifont = &font;
    regular = OpenTextFont(&(struct TextAttr){ "System Mono", 13, 0, 0 });
    bold = OpenTextFont(&(struct TextAttr){ "System Mono", 13, FSF_BOLD, 0 });
    if (!regular || !bold) {
        printf("conwin-test: FAILED: System Mono wouldn't open\n");
        return 1;
    }
    term_font_init(&scalar, &f, regular, bold);
    if (!script_agrees()) {
        printf("conwin-test: FAILED: cmd/scripts/window isn't conformance/shellwin.h's script\n");
        return 1;
    }
    if (!shell("scalar", &scalar, &f, &ready, &done))
        return 1;
    if (ready != sw_crc_ready || done != sw_crc_done) {
        printf("conwin-test: FAILED: the Shell's CRCs are %08x and %08x, not shellwin.h's %08x and %08x\n", ready,
               done, sw_crc_ready, sw_crc_done);
        return 1;
    }
#ifdef GFX_TEST_NEON
    {
        uint32_t nready, ndone;

        if (!shell("NEON", &neon, &f, &nready, &ndone))
            return 1;
        if (nready != ready || ndone != done) {
            printf("conwin-test: FAILED: NEON's Shell isn't scalar's\n");
            return 1;
        }
    }
#endif
    printf("conwin-test: ok: the window test's Shell in System Mono (%ux%u cells), drawn in pieces as whole, "
           "%08x and %08x (build/conwin/*.ppm)\n",
           f.cw, f.ch, ready, done);
    return 0;
}
