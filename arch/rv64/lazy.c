/*
 * arch/rv64/lazy.c -- FP and vector state, switched lazily, on every hart
 *
 * A user task returns to user mode with sstatus.FS and .VS off unless
 * this hart's registers already hold its state. Its first FP or vector
 * instruction then traps as illegal; the kernel decodes it, gives the
 * task a save area (once) and loads it. A write sets FS or VS dirty,
 * which the next trap from user mode notes (and marks clean again, so
 * the hart reports the next write).
 *
 * Saving is eager, at the switch away from the task (arch_switch_out),
 * and only if the state was written: so the registers never hold state a
 * task needs on another hart, and nothing crosses harts but the save
 * area. Loading is lazy, and skipped when this hart still holds the
 * task's state and the task hasn't run elsewhere since (lazy_cpu). A
 * task that never uses FP or vector costs nothing. From omega's
 * arch/riscv64/lazy.c.
 */
#include "arch.h"
#include "cpu.h"
#include "pmm.h"
#include "riscv.h"

constexpr uint64_t SSTATUS_FS_CLEAN = 2ULL << 13;
constexpr uint64_t SSTATUS_FS_DIRTY = 3ULL << 13;
constexpr uint64_t SSTATUS_VS_CLEAN = 2ULL << 9;
constexpr uint64_t SSTATUS_VS_DIRTY = 3ULL << 9;
constexpr size_t FP_WORDS = 33;                 /* f0-f31, fcsr */
constexpr size_t VEC_OFFSET = 64;               /* words: the vector area's start */
static_assert(VEC_OFFSET >= FP_WORDS, "the FP area runs into the vector area");

void riscv_fp_save(uint64_t *area);
void riscv_fp_load(const uint64_t *area);
void riscv_vec_save(uint64_t *area);
void riscv_vec_load(const uint64_t *area);

static uint64_t vlenb;                          /* 0 without V */

/* The vector area holds 4 CSR words and 32 registers; it must fit the page. */
void riscv_lazy_init(uint64_t hart_vlenb)
{
    vlenb = (VEC_OFFSET + 4) * 8 + 32 * hart_vlenb <= PAGE_SIZE ? hart_vlenb : 0;
}

/* What an illegal instruction would have used, had it been allowed: a
 * mask, since vector FP needs both (review H6). */
enum ext { EXT_NONE = 0, EXT_FP = 1, EXT_VEC = 2 };

static unsigned ext_of(uint32_t insn)
{
    uint32_t op = insn & 0x7f, funct3 = (insn >> 12) & 7, csr = insn >> 20;

    if ((insn & 3) != 3) {
        /* Compressed: c.fld, c.fsd (quadrant 0), c.fldsp, c.fsdsp (2). */
        funct3 = (insn >> 13) & 7;
        if (((insn & 3) == 0 || (insn & 3) == 2) && (funct3 == 1 || funct3 == 5))
            return EXT_FP;
        return EXT_NONE;
    }
    switch (op) {
    case 0x07:          /* LOAD-FP and STORE-FP; the other widths are vector */
    case 0x27:
        return funct3 >= 1 && funct3 <= 4 ? EXT_FP : EXT_VEC;
    case 0x43: case 0x47: case 0x4b: case 0x4f: case 0x53:
        return EXT_FP;
    case 0x57:          /* OP-V: OPFVV and OPFVF use the FP state too */
        return funct3 == 1 || funct3 == 5 ? EXT_VEC | EXT_FP : EXT_VEC;
    case 0x73:          /* CSRs: fflags, frm, fcsr; vstart..vcsr, vl, vtype, vlenb */
        if (funct3 == 0 || funct3 == 4)
            return EXT_NONE;
        if (csr >= 0x001 && csr <= 0x003)
            return EXT_FP;
        if ((csr >= 0x008 && csr <= 0x00f) || (csr >= 0xc20 && csr <= 0xc22))
            return EXT_VEC;
        return EXT_NONE;
    default:
        return EXT_NONE;
    }
}

static bool holds(struct arch_context *owner, struct arch_context *ctx)
{
    return owner == ctx && ctx->lazy_cpu == (int)this_cpu()->index;
}

/* On every trap from user mode: note writes since the last trap. */
void riscv_lazy_enter(uint64_t *sstatus, struct arch_context *ctx)
{
    struct arch_cpu *a = &this_cpu()->arch;

    if (a->fp_owner == ctx && (*sstatus & SSTATUS_FS) == SSTATUS_FS_DIRTY) {
        a->fp_dirty = true;
        *sstatus = (*sstatus & ~SSTATUS_FS) | SSTATUS_FS_CLEAN;
    }
    if (a->vec_owner == ctx && (*sstatus & SSTATUS_VS_MASK) == SSTATUS_VS_DIRTY) {
        a->vec_dirty = true;
        *sstatus = (*sstatus & ~SSTATUS_VS_MASK) | SSTATUS_VS_CLEAN;
    }
}

/* Last thing before returning to user mode: access only to what's ours. */
void riscv_lazy_return(uint64_t *sstatus, struct arch_context *ctx)
{
    struct arch_cpu *a = &this_cpu()->arch;

    if (!holds(a->fp_owner, ctx))
        *sstatus &= ~SSTATUS_FS;
    if (!holds(a->vec_owner, ctx))
        *sstatus &= ~SSTATUS_VS_MASK;
}

/* An illegal instruction from user mode: if it's the task's first FP or
 * vector use since it came to this hart, load its state and retry. */
bool riscv_lazy_first_use(uint64_t *sstatus, uint32_t insn, struct arch_context *ctx)
{
    struct cpu *c = this_cpu();
    unsigned need = ext_of(insn);

    if (!vlenb)
        need &= ~EXT_VEC;
    /* Load what's needed and not yet allowed; if that's nothing, the
     * instruction is illegal for another reason. */
    if (*sstatus & SSTATUS_FS)
        need &= ~EXT_FP;
    if (*sstatus & SSTATUS_VS_MASK)
        need &= ~EXT_VEC;
    if (!need)
        return false;
    if (!ctx->lazy) {
        uint64_t page = pmm_alloc();

        if (!page)
            return false;
        ctx->lazy = phys_to_virt(page);
        cpu_count(&this_cpu()->arch.first_uses);
    }
    if (need & EXT_FP) {
        __asm__ volatile("csrs sstatus, %0" : : "r"(SSTATUS_FS_CLEAN) : "memory");
        riscv_fp_load(ctx->lazy);
        __asm__ volatile("csrc sstatus, %0" : : "r"(SSTATUS_FS) : "memory");
        c->arch.fp_owner = ctx;
        c->arch.fp_dirty = false;
        *sstatus |= SSTATUS_FS_CLEAN;
    }
    if (need & EXT_VEC) {
        __asm__ volatile("csrs sstatus, %0" : : "r"(SSTATUS_VS_CLEAN) : "memory");
        riscv_vec_load(ctx->lazy + VEC_OFFSET);
        __asm__ volatile("csrc sstatus, %0" : : "r"(SSTATUS_VS_MASK) : "memory");
        c->arch.vec_owner = ctx;
        c->arch.vec_dirty = false;
        *sstatus |= SSTATUS_VS_CLEAN;
    }
    ctx->lazy_cpu = (int)c->index;
    cpu_count(&this_cpu()->arch.loads);
    return true;
}

void arch_switch_out(struct arch_context *ctx)
{
    struct arch_cpu *a = &this_cpu()->arch;

    if (a->fp_owner == ctx && a->fp_dirty) {
        __asm__ volatile("csrs sstatus, %0" : : "r"(SSTATUS_FS_CLEAN) : "memory");
        riscv_fp_save(ctx->lazy);
        __asm__ volatile("csrc sstatus, %0" : : "r"(SSTATUS_FS) : "memory");
        a->fp_dirty = false;
        cpu_count(&this_cpu()->arch.saves);
    }
    if (a->vec_owner == ctx && a->vec_dirty) {
        __asm__ volatile("csrs sstatus, %0" : : "r"(SSTATUS_VS_CLEAN) : "memory");
        riscv_vec_save(ctx->lazy + VEC_OFFSET);
        __asm__ volatile("csrc sstatus, %0" : : "r"(SSTATUS_VS_MASK) : "memory");
        a->vec_dirty = false;
        cpu_count(&this_cpu()->arch.saves);
    }
}

/* Count a return to user mode that found its state still loaded. */
void riscv_lazy_note_reuse(uint64_t sstatus)
{
    if (sstatus & (SSTATUS_FS | SSTATUS_VS_MASK))
        cpu_count(&this_cpu()->arch.reuses);
}

/* Another hart's owner pointer may still name 'ctx', but lazy_cpu is
 * reset with each new context, so it never matches a later task. */
void arch_context_free(struct arch_context *ctx)
{
    struct arch_cpu *a = &this_cpu()->arch;

    if (a->fp_owner == ctx)
        a->fp_owner = nullptr;
    if (a->vec_owner == ctx)
        a->vec_owner = nullptr;
    if (ctx->lazy)
        pmm_free(virt_to_phys(ctx->lazy));
    ctx->lazy = nullptr;
}

void arch_lazy_counts(struct arch_lazy_counts *c)
{
    *c = (struct arch_lazy_counts){ 0 };
    for (unsigned i = 0; i < ncpus; i++) {
        c->first_uses += atomic_load_explicit(&cpus[i].arch.first_uses, memory_order_relaxed);
        c->loads += atomic_load_explicit(&cpus[i].arch.loads, memory_order_relaxed);
        c->saves += atomic_load_explicit(&cpus[i].arch.saves, memory_order_relaxed);
        c->reuses += atomic_load_explicit(&cpus[i].arch.reuses, memory_order_relaxed);
    }
}
