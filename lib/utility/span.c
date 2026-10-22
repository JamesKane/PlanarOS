/*
 * lib/utility/span.c -- checked slicing and endian loads (docs §17.4.3)
 *
 * Every offset is checked against the span's end with ckd_add, so one
 * read from a file or a firmware table, however large, can't wrap past
 * the check and read outside. Loads are of bytes: unaligned, and Device
 * memory, are both safe.
 */
#include <stdckdint.h>

#include <planar/utility.h>

/* The 'n' bytes at 'offset', if they're all inside. */
static const uint8_t *at(struct Span s, uint64_t offset, uint64_t n)
{
    uint64_t end;

    if (ckd_add(&end, offset, n) || end > s.sp_Len)
        return nullptr;
    return s.sp_Data + offset;
}

bool SpanSlice(struct Span s, uint64_t offset, uint64_t len, struct Span *out)
{
    const uint8_t *p = at(s, offset, len);

    if (!p)
        return false;
    *out = (struct Span){ p, len };
    return true;
}

static uint64_t le(const uint8_t *p, unsigned n)
{
    uint64_t v = 0;

    while (n--)
        v = v << 8 | p[n];
    return v;
}

static uint64_t be(const uint8_t *p, unsigned n)
{
    uint64_t v = 0;

    for (unsigned i = 0; i < n; i++)
        v = v << 8 | p[i];
    return v;
}

#define READER(name, type, bytes, order) \
    bool name(struct Span s, uint64_t offset, type *out) \
    { \
        const uint8_t *p = at(s, offset, bytes); \
\
        if (!p) \
            return false; \
        *out = (type)order(p, bytes); \
        return true; \
    }

READER(SpanReadU8, uint8_t, 1, le)
READER(SpanReadU16LE, uint16_t, 2, le)
READER(SpanReadU32LE, uint32_t, 4, le)
READER(SpanReadU64LE, uint64_t, 8, le)
READER(SpanReadU16BE, uint16_t, 2, be)
READER(SpanReadU32BE, uint32_t, 4, be)
READER(SpanReadU64BE, uint64_t, 8, be)
