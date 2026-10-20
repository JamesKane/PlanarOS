/*
 * arch/aarch64/lazy.c -- FP/SIMD state, switched lazily, on every CPU
 *
 * A user task returns to EL0 with CPACR_EL1.FPEN trapping unless this
 * CPU's registers already hold its state. Its first FP/SIMD instruction
 * then traps (EC 0x07); the kernel gives it a save area (once), loads it
 * and turns access on. AArch64 has no dirty bit, so a task that ran with
 * access on is taken to have written the registers.
 *
 * As on RISC-V (arch/rv64/lazy.c), saving is eager at the switch away
 * and loading lazy, skipped when this CPU still holds the task's state
 * and the task hasn't run elsewhere since. Ordinary code compiled for
 * AArch64 uses SIMD freely (memcpy, struct copies), so the lazy load may
 * rarely be skipped for real programs; the counters say how often
 * (docs §7.4: eager or lazy is decided by measurement).
 */
#include "aarch64.h"
#include "arch.h"
#include "cpu.h"
#include "pmm.h"

constexpr uint64_t CPACR_FPEN = 3ULL << 20;

void aarch64_fp_save(uint64_t *area);
void aarch64_fp_load(const uint64_t *area);


static void fp_access(bool on)
{
    uint64_t cpacr = sysreg_read(cpacr_el1);

    sysreg_write(cpacr_el1, on ? cpacr | CPACR_FPEN : cpacr & ~CPACR_FPEN);
    isb();
}

static bool holds(struct arch_context *ctx)
{
    struct cpu *c = this_cpu();

    return c->arch.fp_owner == ctx && ctx->lazy_cpu == (int)c->index;
}

/* Last thing before returning to EL0: access only if the registers are ours. */
void aarch64_lazy_return(struct arch_context *ctx)
{
    bool ours = holds(ctx);

    fp_access(ours);
    if (ours) {
        this_cpu()->arch.fp_dirty = true;   /* no dirty bit: assume a write */
        cpu_count(&this_cpu()->arch.reuses);
    }
}

/* A trapped FP/SIMD instruction from EL0: load the task's state. */
bool aarch64_lazy_first_use(struct arch_context *ctx)
{
    struct cpu *c = this_cpu();

    if (!ctx->lazy) {
        uint64_t page = pmm_alloc();

        if (!page)
            return false;
        ctx->lazy = phys_to_virt(page);
        cpu_count(&this_cpu()->arch.first_uses);
    }
    fp_access(true);
    aarch64_fp_load(ctx->lazy);
    c->arch.fp_owner = ctx;
    c->arch.fp_dirty = true;
    ctx->lazy_cpu = (int)c->index;
    cpu_count(&this_cpu()->arch.loads);
    return true;
}

void arch_switch_out(struct arch_context *ctx)
{
    struct arch_cpu *a = &this_cpu()->arch;

    if (a->fp_owner == ctx && a->fp_dirty) {
        fp_access(true);
        aarch64_fp_save(ctx->lazy);
        a->fp_dirty = false;
        cpu_count(&this_cpu()->arch.saves);
    }
    fp_access(false);
}

void arch_context_free(struct arch_context *ctx)
{
    struct arch_cpu *a = &this_cpu()->arch;

    if (a->fp_owner == ctx)
        a->fp_owner = nullptr;
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
