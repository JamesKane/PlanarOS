/*
 * arch/aarch64/trap.c -- EL1 exception handling
 *
 * Until the kernel has processes (V1 step 4), every exception is an
 * interrupt, a probe that was allowed to fault, or a kernel fault, which
 * is a Guru Meditation.
 */
#include "aarch64.h"
#include "arch.h"
#include "irq.h"
#include "kprintf.h"

struct trap_frame {
    uint64_t x[31];
    uint64_t sp;        /* at the time of the exception */
    uint64_t elr;
    uint64_t spsr;
};

struct fixup {
    uint64_t insn;
    uint64_t resume;
};

extern const struct fixup __fixup_start[], __fixup_end[];

void trap_sync(struct trap_frame *f, uint64_t kind);
void trap_irq(struct trap_frame *f, uint64_t kind);
void trap_unexpected(struct trap_frame *f, uint64_t kind);

[[noreturn]] static void guru(struct trap_frame *f, uint64_t kind)
{
    uint64_t esr = sysreg_read(esr_el1);

    /* A kernel fault is a real Guru Meditation (omega arch §11.1). */
    kprintf("\nGuru Meditation #%llx.%llx (vector %llu)\n", (unsigned long long)esr,
            (unsigned long long)f->elr, (unsigned long long)kind);
    kprintf("  esr %p  far %p\n", (void *)esr, (void *)sysreg_read(far_el1));
    kprintf("  elr %p  lr  %p  sp %p\n", (void *)f->elr, (void *)f->x[30],
            (void *)f->sp);
    arch_halt();
}

void trap_sync(struct trap_frame *f, uint64_t kind)
{
    for (const struct fixup *e = __fixup_start; e < __fixup_end; e++) {
        if (e->insn == f->elr) {
            f->x[0] = sysreg_read(esr_el1);
            f->elr = e->resume;
            return;
        }
    }
    guru(f, kind);
}

void trap_irq(struct trap_frame *, uint64_t)
{
    irq_external();
}

void trap_unexpected(struct trap_frame *f, uint64_t kind)
{
    guru(f, kind);
}

bool arch_probe_read(uint64_t addr, uint64_t *val)
{
    return probe_ld(addr, val) == 0;
}
