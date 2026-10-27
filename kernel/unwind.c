/*
 * kernel/unwind.c -- backtraces of user code, the portable half
 * (kernel/unwind.h)
 */
#include <planar/endian.h>

#include "arch.h"
#include "kprintf.h"
#include "library.h"
#include "process.h"
#include "unwind.h"

constexpr unsigned MAX_FRAMES = 32;

void uimage_init(struct uimage *im, uint64_t base, uint64_t text, uint64_t unwind,
                 uint64_t unwind_end, uint64_t note, uint64_t note_end,
                 bool (*read)(void *ctx, uint64_t off, void *dst, uint64_t n), void *ctx)
{
    uint8_t n[32];

    *im = (struct uimage){ .base = base, .text = text };
    if (unwind < unwind_end && unwind_end <= text && unwind % 4 == 0 && (unwind_end - unwind) % 8 == 0 &&
        (unwind_end - unwind) / 8 <= UINT32_MAX) {
        im->unwind = base + unwind;
        im->entries = (uint32_t)((unwind_end - unwind) / 8);
    }
    /* NT_GNU_BUILD_ID: namesz 4, descsz 16, type 3, "GNU", the id. */
    if (note < note_end && note_end <= text && note_end - note >= sizeof n && read(ctx, note, n, sizeof n) &&
        get_le32(n) == 4 && get_le32(n + 4) == 16 && get_le32(n + 8) == 3 && n[12] == 'G' && n[13] == 'N' &&
        n[14] == 'U' && n[15] == 0) {
        for (unsigned i = 0; i < 16; i++)
            im->id[i] = n[16 + i];
        im->has_id = true;
    }
}

bool unwind_read(const struct unwinder *u, uint64_t addr, uint64_t *v)
{
    if (addr % 8 || addr < u->stack_lo || addr >= u->stack_hi || u->stack_hi - addr < 8)
        return false;
    return arch_copy_from_user(v, addr, 8);
}

bool unwind_code(uint64_t addr, uint32_t *w)
{
    return addr % 4 == 0 && arch_copy_from_user(w, addr, 4);
}

/* The entry covering pc in 'im': its start (an address) and word; false
 * if there's none (no table, or pc past its last function). */
static bool entry_at(const struct uimage *im, uint64_t pc, uint64_t *start, uint32_t *info)
{
    uint32_t lo = 0, hi = im->entries, e[2];

    if (!im->entries || pc < im->base || pc - im->base >= im->text)
        return false;
    while (lo < hi) {                   /* the first entry starting after pc */
        uint32_t mid = lo + (hi - lo) / 2;

        if (!arch_copy_from_user(e, im->unwind + 8ull * mid, 8))
            return false;
        if (im->base + e[0] <= pc)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (!lo || lo == im->entries || !arch_copy_from_user(e, im->unwind + 8ull * (lo - 1), 8))
        return false;
    *start = im->base + e[0];
    *info = e[1];
    return true;
}

/* The image holding pc: the program's, or a library's. */
static const struct uimage *image_at(struct process *p, uint64_t pc, const char **name)
{
    if (pc >= p->image.base && pc - p->image.base < p->image.text) {
        *name = p->name;
        return &p->image;
    }
    return library_image_at(pc, name);
}

unsigned unwind_record(struct process *p, struct uframe top, uint64_t stack_lo, uint64_t stack_hi,
                       uint64_t *pcs, unsigned max)
{
    struct unwinder u = { stack_lo, stack_hi };
    struct uframe f = top;
    uint64_t pc[MAX_FRAMES];
    const struct uimage *im[MAX_FRAMES];
    const char *name[MAX_FRAMES];
    unsigned n = 0;

    while (n < MAX_FRAMES) {
        uint64_t start = 0;
        uint32_t info = UW_UNKNOWN;
        bool first = n == 0;

        pc[n] = f.pc;
        im[n] = image_at(p, first ? f.pc : f.pc - 1, &name[n]);
        if (im[n] && !entry_at(im[n], first ? f.pc : f.pc - 1, &start, &info))
            info = UW_UNKNOWN;
        /* Stopped outside every image: a call or jump into nowhere, just
         * made, so the caller is in the return-address register. */
        if (!im[n] && first)
            info = UW_FRAMELESS;
        n++;
        if (!arch_unwind_step(&u, &f, first, start, info))
            break;
    }
    /* Each image the backtrace touched, once, then the frames. */
    for (unsigned i = 0; i < n; i++) {
        bool seen = !im[i];

        for (unsigned j = 0; j < i && !seen; j++)
            seen = im[j] == im[i];
        if (seen)
            continue;
        char id[33] = "-";
        for (unsigned k = 0; im[i]->has_id && k < 16; k++) {
            id[2 * k] = "0123456789abcdef"[im[i]->id[k] >> 4];
            id[2 * k + 1] = "0123456789abcdef"[im[i]->id[k] & 15];
            id[2 * k + 2] = '\0';
        }
        kprintf("planar: %s: image %s %s at %p\n", p->name, name[i], id, (void *)im[i]->base);
    }
    for (unsigned i = 0; i < n; i++)
        if (im[i])
            kprintf("planar: %s: #%u %p %s+0x%llx\n", p->name, i, (void *)pc[i], name[i],
                    (unsigned long long)(pc[i] - im[i]->base));
        else
            kprintf("planar: %s: #%u %p\n", p->name, i, (void *)pc[i]);
    for (unsigned i = 0; i < n && i < max; i++)
        pcs[i] = pc[i];
    return n;
}
