/*
 * arch/rv64/unwind.c -- one frame of a user backtrace (kernel/unwind.h)
 *
 * planar-c's prologue is addi sp, sp, -F; sd ra; sd s0; sd each saved
 * register; addi s0, sp, F - va (va: 64 for a variadic function's save
 * area). s0 is the frame pointer from then on, with ra at s0 - 8, the
 * caller's s0 at s0 - 16, and the caller's sp at s0 + va. Before that,
 * the caller's sp is sp + F, F read from the prologue's first
 * instruction. The epilogue reloads ra and s0 before the saved
 * registers, so from just after ld s0 to the ret, sp + F is the caller's
 * sp again: recognized by matching what's left of the epilogue. clang's
 * code (UNKNOWN) keeps the same record, for the chain.
 */
#include "unwind.h"

constexpr uint32_t RET = 0x00008067;

static uint32_t itype(uint32_t op, uint32_t f3, uint32_t rd, uint32_t rs1, int32_t imm)
{
    return (uint32_t)imm << 20 | rs1 << 15 | f3 << 12 | rd << 7 | op;
}

/* The saved registers' mask bits, in order: s1-s11, then fs0-fs11. */
static const uint8_t GPRS[] = { 9, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27 };
static const uint8_t FPRS[] = { 8, 9, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27 };

/* The caller's frame from the record below fp: {s0, ra} at fp - 16. */
static bool record(const struct unwinder *u, struct uframe *c, uint64_t fp, uint64_t va)
{
    if (!unwind_read(u, fp - 16, &c->fp) || !unwind_read(u, fp - 8, &c->pc))
        return false;
    c->sp = fp + va;
    return true;
}

/* Is pc in what's left of an epilogue after its ld s0: the saved
 * registers' loads, addi sp, sp, F? */
static bool after_s0(uint64_t pc, uint32_t info, uint64_t frame, uint64_t first)
{
    uint32_t tail[24], n = 0, w;

    for (unsigned i = 0; i < sizeof GPRS; i++)
        if (uw_gprs(info) >> i & 1)
            tail[n] = itype(0x03, 3, GPRS[i], 2, (int32_t)(first - 24 - 8 * n)), n++;
    for (unsigned i = 0; i < sizeof FPRS; i++)
        if (uw_fprs(info) >> i & 1)
            tail[n] = itype(0x07, 3, FPRS[i], 2, (int32_t)(first - 24 - 8 * n)), n++;
    tail[n++] = itype(0x13, 0, 2, 2, (int32_t)frame);
    for (uint32_t j = 0; j < n; j++) {
        bool match = true;

        for (uint32_t q = j; q < n && match; q++)
            match = unwind_code(pc + 4 * (q - j), &w) && w == tail[q];
        if (match)
            return true;
    }
    return false;
}

bool arch_unwind_step(const struct unwinder *u, struct uframe *f, bool top, uint64_t start,
                      uint32_t info)
{
    struct uframe c = *f;
    uint64_t n = (f->pc - start) / 4, va = info & UW_VA ? 64 : 0;
    uint32_t insn;

    switch (uw_kind(info)) {
    case UW_GAP:
        return false;
    case UW_FRAMELESS:
        if (!top)
            return false;           /* it calls nothing */
        c.pc = f->ra;
        break;
    case UW_FRAME: {
        uint64_t frame;

        if (top && (n == 0 || (unwind_code(f->pc, &insn) && insn == RET))) {
            c.pc = f->ra;
            break;
        }
        if (!unwind_code(start, &insn))
            return false;
        frame = (uint64_t)-(int64_t)((int32_t)insn >> 20);
        if (top && (n < uw_prologue(info) || after_s0(f->pc, info, frame, frame - va))) {
            c.pc = f->ra;
            c.sp = f->sp + frame;
        } else if (!record(u, &c, f->fp, va)) {
            return false;
        }
        break;
    }
    default:
        if (!record(u, &c, f->fp, 0) || c.sp <= f->sp)
            return false;
    }
    if (!c.pc)
        return false;               /* the outermost frame */
    c.ra = 0;
    *f = c;
    return true;
}
