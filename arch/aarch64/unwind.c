/*
 * arch/aarch64/unwind.c -- one frame of a user backtrace (kernel/unwind.h)
 *
 * planar-c's prologue is stp x29, x30, [sp, #-16]!; mov x29, sp; then sp
 * moves down and the callee-saved registers go below the frame record.
 * Its epilogue restores them, puts sp back, ldp x29, x30, [sp], #16; ret.
 * So x29 is the frame pointer from the second instruction until the last
 * but one, and the caller's sp is 16 above it. Before the first and at
 * the ret, nothing is pushed; after the first alone, only the record.
 * clang's code (UNKNOWN) keeps the same record, for the chain.
 */
#include "unwind.h"

constexpr uint32_t RET = 0xD65F03C0;

/* The caller's frame from the frame record {x29, x30} at fp. */
static bool record(const struct unwinder *u, struct uframe *c, uint64_t fp)
{
    if (!unwind_read(u, fp, &c->fp) || !unwind_read(u, fp + 8, &c->pc))
        return false;
    c->sp = fp + 16;
    return true;
}

bool arch_unwind_step(const struct unwinder *u, struct uframe *f, bool top, uint64_t start,
                      uint32_t info)
{
    struct uframe c = *f;
    uint64_t n = (f->pc - start) / 4;
    uint32_t insn;

    switch (uw_kind(info)) {
    case UW_GAP:
        return false;
    case UW_FRAMELESS:
        if (!top)
            return false;           /* it calls nothing */
        c.pc = f->ra;
        break;
    case UW_FRAME:
        if (top && (n == 0 || (unwind_code(f->pc, &insn) && insn == RET))) {
            c.pc = f->ra;
        } else if (top && n == 1) {
            c.pc = f->ra;
            c.sp = f->sp + 16;
        } else if (!record(u, &c, f->fp)) {
            return false;
        }
        break;
    default:
        if (!record(u, &c, f->fp) || c.sp <= f->sp)
            return false;
    }
    c.pc &= 0x0000FFFFFFFFFFFFull;  /* PAC bits, if any */
    if (!c.pc)
        return false;               /* the outermost frame */
    c.ra = 0;
    *f = c;
    return true;
}
