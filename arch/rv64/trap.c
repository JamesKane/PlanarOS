/*
 * arch/rv64/trap.c -- S-mode trap handling
 */
#include "arch.h"
#include "cpu.h"
#include "irq.h"
#include "kprintf.h"
#include "pmm.h"
#include "process.h"
#include "riscv.h"
#include "sched.h"
#include "syscall.h"

constexpr uint64_t CAUSE_ECALL_U = 8;

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
[[noreturn]] void trap_resume(struct trap_frame *f);
uint64_t probe_copy_from_user(void *dst, uint64_t src, size_t n);
uint64_t probe_copy_to_user(uint64_t dst, const void *src, size_t n);

void trap_init(void)
{
    csr_write_stvec((uint64_t)(uintptr_t)trap_entry);
}

/* What a user-mode exception means. */
static enum fault fault_kind(uint64_t scause)
{
    switch (scause) {
    case 1: case 12:            return FAULT_EXEC;
    case 5: case 13:            return FAULT_READ;
    case 7: case 15:            return FAULT_WRITE;
    case 2:                     return FAULT_ILLEGAL;
    case 3:                     return FAULT_BREAK;
    case 0: case 4: case 6:     return FAULT_ALIGN;
    default:                    return FAULT_OTHER;
    }
}

/* The instruction at 'pc' in user memory, for an illegal-instruction
 * trap that didn't leave it in stval. 0 if unreadable. */
static uint32_t user_insn(uint64_t pc, uint64_t stval)
{
    uint16_t half[2] = { 0, 0 };

    if (stval)
        return (uint32_t)stval;
    if (!arch_copy_from_user(&half[0], pc, 2))
        return 0;
    if ((half[0] & 3) == 3 && !arch_copy_from_user(&half[1], pc + 2, 2))
        return 0;
    return (uint32_t)half[1] << 16 | half[0];
}

static void user_trap(struct trap_frame *f, uint64_t scause, uint64_t stval)
{
    if (scause == CAUSE_ECALL_U) {
        f->sepc += 4;
        arch_irq_enable();
        f->x[10] = (uint64_t)syscall_dispatch(f->x[17], &f->x[10]);
        arch_irq_disable();
        return;
    }
    if ((scause == 12 || scause == 13 || scause == 15) && riscv_fault_spurious(stval, scause))
        return;
    if (scause == 2 && riscv_lazy_first_use(&f->sstatus, user_insn(f->sepc, stval),
                                            &task_self()->ctx))
        return;             /* run it again, now allowed */
    process_fault(fault_kind(scause), f->sepc, stval);
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
    bool from_user = !(f->sstatus & SSTATUS_SPP);

    if (from_user)
        riscv_lazy_enter(&f->sstatus, &task_self()->ctx);
    if (scause & SCAUSE_INTERRUPT) {
        irq_enter();
        if (scause == (SCAUSE_INTERRUPT | IRQ_S_TIMER)) {
            irq_timer();
        } else if (scause == (SCAUSE_INTERRUPT | IRQ_S_SOFT)) {
            __asm__ volatile("csrc sip, %0" : : "r"(SIE_SSIE) : "memory");
            irq_ipi();
        } else if (scause == (SCAUSE_INTERRUPT | IRQ_S_EXTERNAL)) {
            irq_external();
        }
        irq_exit();         /* may switch tasks: this frame waits on its stack */
    } else if (from_user) {
        user_trap(f, scause, stval);
    } else {
        kernel_trap(f, scause, stval);
    }
    if (from_user) {
        user_return();      /* a killed task stops here */
        riscv_lazy_return(&f->sstatus, &task_self()->ctx);
        riscv_lazy_note_reuse(f->sstatus);
        if (f->sepc >= ARCH_USER_TOP) {
            /* Returning to user mode at a kernel address: the frame was
             * overwritten. Always a kernel bug; say where. */
            struct task *t = task_self();

            kprintf("\nGuru Meditation: return to user at %p\n", (void *)f->sepc);
            kprintf("  frame %p  task %p (%s)  cpu %u  stack %p-%p\n", (void *)f, (void *)t,
                    t->name, this_cpu()->index, phys_to_virt(t->stack),
                    phys_to_virt(t->stack + TASK_STACK_PAGES * PAGE_SIZE));
            arch_halt();
        }
    }
}

/* The frame is built on the current stack; once popped, the slot above
 * it is where the next trap from user mode lands. */
[[noreturn]] void arch_user_enter(uint64_t pc, uint64_t sp, uint64_t arg0, uint64_t arg1)
{
    struct trap_frame f = { 0 };

    f.x[2] = sp;
    f.x[10] = arg0;
    f.x[11] = arg1;
    f.sepc = pc;
    /* User mode, interrupts on after sret (SPIE) but not before (SIE:
     * they're still on here, and trap_return writes this sstatus before
     * its sret), FP and vector off. */
    f.sstatus = (csr_read_sstatus() & ~(SSTATUS_SIE | SSTATUS_SPP | SSTATUS_SUM |
                                        SSTATUS_FS | SSTATUS_VS_MASK)) | SSTATUS_SPIE;
    arch_irq_disable();
    trap_resume(&f);
}

bool arch_copy_from_user(void *dst, uint64_t src, size_t n)
{
    uint64_t cause;

    if (src >= ARCH_USER_TOP || n > ARCH_USER_TOP - src)
        return false;
    __asm__ volatile("csrs sstatus, %0" : : "r"(SSTATUS_SUM) : "memory");
    cause = probe_copy_from_user(dst, src, n);
    __asm__ volatile("csrc sstatus, %0" : : "r"(SSTATUS_SUM) : "memory");
    return cause == 0;
}

bool arch_copy_to_user(uint64_t dst, const void *src, size_t n)
{
    uint64_t cause;

    if (dst >= ARCH_USER_TOP || n > ARCH_USER_TOP - dst)
        return false;
    __asm__ volatile("csrs sstatus, %0" : : "r"(SSTATUS_SUM) : "memory");
    cause = probe_copy_to_user(dst, src, n);
    __asm__ volatile("csrc sstatus, %0" : : "r"(SSTATUS_SUM) : "memory");
    return cause == 0;
}

bool arch_probe_read(uint64_t addr, uint64_t *val)
{
    return probe_ld(addr, val) == 0;
}
