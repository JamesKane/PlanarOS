/*
 * arch/rv64/trap.c -- S-mode trap handling
 *
 * Until the kernel has processes (V1 step 4), every trap is an interrupt,
 * a probe that was allowed to fault, or a kernel fault, which is a Guru
 * Meditation.
 */
#include "arch.h"
#include "irq.h"
#include "kprintf.h"
#include "riscv.h"
#include "sched.h"

struct trap_frame {
    uint64_t x[32];     /* x[2] is the sp at the time of the trap */
    uint64_t sepc;
    uint64_t sstatus;
};

struct fixup {
    uint64_t insn;
    uint64_t resume;
};

extern const struct fixup __fixup_start[], __fixup_end[];
extern const char trap_entry[];

void trap_handler(struct trap_frame *f);

void trap_init(void)
{
    csr_write_stvec((uint64_t)(uintptr_t)trap_entry);
}

static void kernel_trap(struct trap_frame *f, uint64_t scause, uint64_t stval)
{
    for (const struct fixup *e = __fixup_start; e < __fixup_end; e++) {
        if (e->insn == f->sepc) {
            f->x[10] = scause;
            f->sepc = e->resume;
            return;
        }
    }

    /* A kernel fault is a real Guru Meditation (omega arch §11.1). */
    kprintf("\nGuru Meditation #%llx.%llx\n", (unsigned long long)scause,
            (unsigned long long)f->sepc);
    kprintf("  scause %p  stval %p\n", (void *)scause, (void *)stval);
    kprintf("  sepc   %p  ra    %p  sp %p\n", (void *)f->sepc, (void *)f->x[1],
            (void *)f->x[2]);
    arch_halt();
}

void trap_handler(struct trap_frame *f)
{
    uint64_t scause = csr_read_scause();
    uint64_t stval = csr_read_stval();

    if (!(scause & SCAUSE_INTERRUPT)) {
        kernel_trap(f, scause, stval);
        return;
    }
    irq_enter();
    if (scause == (SCAUSE_INTERRUPT | IRQ_S_TIMER)) {
        irq_timer();
    } else if (scause == (SCAUSE_INTERRUPT | IRQ_S_SOFT)) {
        __asm__ volatile("csrc sip, %0" : : "r"(SIE_SSIE) : "memory");
        irq_ipi();
    } else if (scause == (SCAUSE_INTERRUPT | IRQ_S_EXTERNAL)) {
        irq_external();
    }
    irq_exit();             /* may switch tasks: this frame waits on its stack */
}

bool arch_probe_read(uint64_t addr, uint64_t *val)
{
    return probe_ld(addr, val) == 0;
}
