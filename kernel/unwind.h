/*
 * kernel/unwind.h -- backtraces of user code, for crash records (docs §19.5)
 *
 * Every image the kernel loads (a program, a library) carries planar-c's
 * unwind table (planar-c docs/debug-info-design.md §5.1: one 8-byte entry
 * per function, sorted) and a build-id, which its header locates: crt0's
 * for a program, lib/libhead.S's for a library. The kernel records them as
 * a uimage. A fault then prints the images its backtrace touches, each
 * with its build-id, and one line per frame as image + offset, which
 * planar-symbolize turns into functions and source lines on the host.
 *
 * kernel/unwind.c is the portable half: images, the table lookup, the
 * checked reads, the record. arch/<isa>/unwind.c steps one frame by that
 * ISA's prologue and epilogue shapes, as planar-c's dbg_unwind_step does.
 * Code with no entry, or an UNKNOWN one (assembly, clang's), is followed
 * by the frame-pointer chain while it stays inside the task's stack.
 */
#ifndef KERNEL_UNWIND_H
#define KERNEL_UNWIND_H

#include <stdint.h>

struct process;

struct uimage {
    uint64_t base;                  /* where it is                       */
    uint64_t text;                  /* bytes of code and constants       */
    uint64_t unwind;                /* the table's address, or 0         */
    uint32_t entries;
    bool has_id;
    uint8_t id[16];                 /* planar-ld's, or ld.lld --build-id=md5 */
};

/* Record an image at 'base' with 'text' bytes of code: its table at
 * offsets [unwind, unwind_end), and its build-id note at [note,
 * note_end), read through 'read' (offsets from base). Any field that
 * doesn't fit inside the code is left out. */
void uimage_init(struct uimage *im, uint64_t base, uint64_t text, uint64_t unwind,
                 uint64_t unwind_end, uint64_t note, uint64_t note_end,
                 bool (*read)(void *ctx, uint64_t off, void *dst, uint64_t n), void *ctx);

/* The unwind word's fields (planar-c's dbg.h). */
enum : uint32_t { UW_FRAMELESS, UW_FRAME, UW_UNKNOWN, UW_GAP };
enum : uint32_t { UW_VA = 1u << 7, UW_DYN = 1u << 8 };
static inline uint32_t uw_kind(uint32_t w) { return w & 3; }
static inline uint32_t uw_prologue(uint32_t w) { return w >> 2 & 31; }
static inline uint32_t uw_gprs(uint32_t w) { return w >> 9 & 0x7FF; }
static inline uint32_t uw_fprs(uint32_t w) { return w >> 20 & 0xFFF; }

struct uframe {
    uint64_t pc, sp, fp, ra;        /* ra: the return-address register's */
};

/* One walk: the task's stack bounds, which every stack read must be
 * inside. */
struct unwinder {
    uint64_t stack_lo, stack_hi;
};

bool unwind_read(const struct unwinder *u, uint64_t addr, uint64_t *v);
bool unwind_code(uint64_t addr, uint32_t *w);

/* One frame of 'f' (its pc in a function whose entry starts at 'start',
 * with unwind word 'info'; UW_UNKNOWN if none) to its caller's. 'top':
 * f is where the task stopped; otherwise f->pc is a return address.
 * False when the walk ends. */
bool arch_unwind_step(const struct unwinder *u, struct uframe *f, bool top, uint64_t start,
                      uint32_t info);

/* Print a faulting task's backtrace from 'top', as PlanarOS §19.5 has it,
 * under the process's name; the first 'max' frames' pcs go in 'pcs'.
 * Returns how many frames there were. */
unsigned unwind_record(struct process *p, struct uframe top, uint64_t stack_lo, uint64_t stack_hi,
                       uint64_t *pcs, unsigned max);

#endif /* KERNEL_UNWIND_H */
