/*
 * conformance/utility.c -- utility.library (V6.3)
 *
 * conformance/utilcheck.h's cases: tag lists through TAG_MORE, TAG_SKIP
 * and TAG_IGNORE, hooks, case for every byte, SNPrintf cut to fit, Str's
 * helpers and StrToU64's edges, and the Span readers at the end, past
 * it and at offsets that would wrap. Then what the common calls cost.
 * Exits 0, or 1 with the failing line logged.
 */
#include "log.h"
#include "utilcheck.h"

static const uint8_t table[] = { 'h', 'e', 'a', 'd', 0x00, 0x01, 0x00, 0x00, 0x12, 0x34, 0x56,
                                 0x78 };

/* What each costs, in nanoseconds. */
static void costs(void)
{
    constexpr unsigned N = 10000;
    static const struct TagItem tags[] = {
        { TAG_USER + 1, 1 }, { TAG_USER + 2, 2 }, { TAG_USER + 3, 3 }, { TAG_USER + 4, 4 },
        { TAG_USER + 5, 5 }, { TAG_USER + 6, 6 }, { TAG_USER + 7, 7 }, { TAG_USER + 8, 8 },
        { TAG_DONE, 0 },
    };
    struct Span s = { table, sizeof table };
    volatile uint64_t sink = 0;
    uint64_t t0, tag, fmt, read;
    char line[64];
    uint32_t w;

    t0 = CurrentTime();
    for (unsigned i = 0; i < N; i++)
        sink += GetTagData(TAG_USER + 8, 0, tags);
    tag = (CurrentTime() - t0) / N;
    t0 = CurrentTime();
    for (unsigned i = 0; i < N; i++)
        sink += (uint64_t)SNPrintf(line, sizeof line, "%s %d at %#lx", "block", (int)i, t0);
    fmt = (CurrentTime() - t0) / N;
    t0 = CurrentTime();
    for (unsigned i = 0; i < N; i++)
        if (SpanReadU32BE(s, 8 - (i & 1) * 4, &w))
            sink += w;
    read = (CurrentTime() - t0) / N;
    KPrintF("utility: GetTagData of the 8th tag %lu ns, SNPrintf of a short line %lu ns, "
            "SpanReadU32BE %lu ns", tag, fmt, read);
}

int main(void)
{
    unsigned cases = 0, line;

    if ((line = util_check(&cases))) {
        KPrintF("utility: check at utilcheck.h:%u failed", line);
        return 1;
    }
    KPrintF("utility: %u cases held: tags, hooks, case, SNPrintf, Str and Span", cases);
    costs();
    return 0;
}
