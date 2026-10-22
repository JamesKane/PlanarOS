/*
 * lib/gfx/test.c -- gfx.library on the host
 *
 * lib/gfx's files with the sanitizers, against conformance/gfxcheck.h's
 * reference, through two tables: the scalar entries, and, where the host
 * is an Arm one (GFX_TEST_NEON), arch/aarch64/lib/gfx.c's NEON entries.
 * RVV's are checked in the guest, by conformance/gfx.c: macOS has no
 * QEMU user mode to run them here. Areas are the host's memory, page
 * aligned and zeroed, and every one must be freed by the end.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gfxcheck.h"
#include "rows.h"

static struct {
    void *base;
    uint64_t size;
    uint32_t flags;
} areas[16];
static unsigned live;

int64_t AllocArea(uint64_t size, uint32_t flags, void **address)
{
    size = (size + 4095) & ~4095ull;
    for (unsigned i = 0; i < sizeof areas / sizeof areas[0]; i++)
        if (!areas[i].base) {
            if (!(areas[i].base = aligned_alloc(4096, size)))
                return ERR_NOMEM;
            memset(areas[i].base, 0, size);
            areas[i].size = size;
            areas[i].flags = flags;
            *address = areas[i].base;
            live++;
            return i + 1;
        }
    return ERR_NOMEM;
}

int64_t FreeArea(Handle h)
{
    if (h < 1 || h > sizeof areas / sizeof areas[0] || !areas[h - 1].base)
        return ERR_BADHANDLE;
    free(areas[h - 1].base);
    areas[h - 1].base = nullptr;
    live--;
    return 0;
}

int64_t QueryArea(const void *address, struct AreaInfo *info)
{
    for (unsigned i = 0; i < sizeof areas / sizeof areas[0]; i++) {
        uint8_t *b = areas[i].base;

        if (b && (const uint8_t *)address >= b && (const uint8_t *)address < b + areas[i].size) {
            *info = (struct AreaInfo){ .ai_Address = b, .ai_Size = areas[i].size, .ai_Area = i + 1,
                                       .ai_Rights = AREAR_READ | AREAR_WRITE, .ai_Flags = areas[i].flags,
                                       .ai_Sharers = 1 };
            return 0;
        }
    }
    return ERR_NOTFOUND;
}

static int run(const char *name, const struct GfxIFace *g)
{
    unsigned cases = 0, line = gc_all(g, &cases);

    if (line) {
        printf("gfx-test: FAILED: %s: gfxcheck.h:%u (width %d, at %d, place %d)\n", name, line, gc_w,
               gc_a, gc_p);
        return 1;
    }
    if (live) {
        printf("gfx-test: FAILED: %s: %u Areas not freed\n", name, live);
        return 1;
    }
    printf("gfx-test: ok: %s: %u cases\n", name, cases);
    return 0;
}

int main(void)
{
    static const struct GfxIFace scalar = {
        .AllocBitMap = AllocBitMap, .FreeBitMap = FreeBitMap, .GetBitMapAttr = GetBitMapAttr,
        .InitBitMap = InitBitMap, .InitRastPort = InitRastPort,
        .RectFill = RectFill, .BltBitMap = BltBitMap, .DrawBevel = DrawBevel, .Text = Text,
    };
    struct GfxIFace vector = scalar;

    if (run("scalar", &scalar))
        return 1;
#ifdef GFX_TEST_NEON
    vector.RectFill = RectFillVector;
    vector.BltBitMap = BltBitMapVector;
    vector.DrawBevel = DrawBevelVector;
    return run("NEON", &vector);
#else
    (void)vector;
    printf("gfx-test: no vector entries for this host: scalar only\n");
    return 0;
#endif
}
