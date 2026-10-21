/*
 * servers/acpid/heap.c -- ACPICA's memory, on Planar
 *
 * Blocks are powers of two from 32 bytes to 64 KiB, each with a 16-byte
 * header saying which, carved from Areas of 1 MiB and kept on a free
 * list per size when freed. ACPICA allocates small objects by the
 * thousand and keeps its own caches of them, so nothing coalesces. A
 * block bigger than 64 KiB is an Area of its own, freed with it.
 */
#include <planar/exec.h>

#include "acpid.h"

void *memset(void *dst, int c, size_t n);

constexpr unsigned MIN_SHIFT = 5, MAX_SHIFT = 16, HEADER = 16;
constexpr uint64_t CHUNK = 1 << 20;
constexpr uint32_t BIG = 0xff;

struct header {
    uint32_t shift;                 /* the block's size, or BIG */
    Handle area;                    /* BIG's own Area */
    struct header *next;            /* on its free list */
};

static struct header *free_list[MAX_SHIFT + 1];
static uint8_t *chunk_at;
static uint64_t chunk_left;

void *acpid_alloc(size_t n)
{
    unsigned shift = MIN_SHIFT;
    uint64_t want = n + HEADER;
    struct header *h;

    if (want > 1u << MAX_SHIFT) {
        void *at;
        int64_t a = AllocArea((want + 4095) & ~4095ULL, AREAF_CLEAR, &at);

        if (a < 0)
            return nullptr;
        h = at;
        h->shift = BIG;
        h->area = (Handle)a;
        return (uint8_t *)h + HEADER;
    }
    while ((1ULL << shift) < want)
        shift++;
    if ((h = free_list[shift])) {
        free_list[shift] = h->next;
    } else {
        if (chunk_left < 1ULL << shift) {
            void *at;

            if (AllocArea(CHUNK, AREAF_CLEAR, &at) < 0)
                return nullptr;
            chunk_at = at;
            chunk_left = CHUNK;
        }
        h = (struct header *)chunk_at;
        chunk_at += 1ULL << shift;
        chunk_left -= 1ULL << shift;
    }
    h->shift = shift;
    h->next = nullptr;
    memset((uint8_t *)h + HEADER, 0, (1ULL << shift) - HEADER);
    return (uint8_t *)h + HEADER;
}

void acpid_free(void *p)
{
    struct header *h;

    if (!p)
        return;
    h = (struct header *)((uint8_t *)p - HEADER);
    if (h->shift == BIG) {
        FreeArea(h->area);
        return;
    }
    h->next = free_list[h->shift];
    free_list[h->shift] = h;
}
