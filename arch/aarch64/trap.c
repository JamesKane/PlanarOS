/*
 * arch/aarch64/trap.c -- EL1 exception handling
 *
 * From EL1: interrupts, probes that were allowed to fault, and kernel
 * faults, which are a Guru Meditation. From EL0: kernel calls (SVC),
 * faults that end the process, and interrupts.
 */
#include "aarch64.h"
#include "arch.h"
#include "area.h"
#include "irq.h"
#include "kprintf.h"
#include "process.h"
#include "sched.h"
#include "syscall.h"

struct trap_frame {
    uint64_t x[31];
    uint64_t sp;        /* at the time of the exception; SP_EL0 from EL0 */
    uint64_t elr;
    uint64_t spsr;
};

struct fixup {
    uint64_t insn;
    uint64_t resume;
};

/* ESR_EL1.EC values from EL0. */
constexpr unsigned EC_UNKNOWN    = 0x00;
constexpr unsigned EC_FP         = 0x07;
constexpr unsigned EC_SVC64      = 0x15;
constexpr unsigned EC_SYSREG      = 0x18;   /* MSR/MRS/SYS trapped: EL1's registers */
constexpr unsigned EC_SVE         = 0x19;   /* SVE while CPACR_EL1.ZEN traps it */
constexpr unsigned EC_IABT_LOWER = 0x20;
constexpr unsigned EC_PC_ALIGN   = 0x22;
constexpr unsigned EC_DABT_LOWER = 0x24;
constexpr unsigned EC_SP_ALIGN   = 0x26;
constexpr unsigned EC_BRK        = 0x3c;

extern const struct fixup __fixup_start[], __fixup_end[];

void trap_sync(struct trap_frame *f, uint64_t kind);
void trap_irq(struct trap_frame *f, uint64_t kind);
void trap_el0_sync(struct trap_frame *f, uint64_t kind);
void trap_el0_irq(struct trap_frame *f, uint64_t kind);
void trap_unexpected(struct trap_frame *f, uint64_t kind);
[[noreturn]] void trap_resume(struct trap_frame *f);
uint64_t probe_copy_from_user(void *dst, uint64_t src, size_t n);
uint64_t probe_copy_to_user(uint64_t dst, const void *src, size_t n);

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
    irq_enter();
    irq_external();
    irq_exit();             /* may switch tasks: this frame waits on its stack */
}

void trap_el0_sync(struct trap_frame *f, uint64_t)
{
    uint64_t esr = sysreg_read(esr_el1), far = sysreg_read(far_el1);
    unsigned ec = (unsigned)(esr >> 26) & 0x3f;

    switch (ec) {
    case EC_SVC64:
        arch_irq_enable();
        f->x[0] = (uint64_t)syscall_dispatch(f->x[8], &f->x[0]);
        arch_irq_disable();
        break;
    case EC_DABT_LOWER:
        if (area_fault_retry(far, esr & (1u << 6) ? MAP_WRITE : MAP_READ))
            break;
        process_fault(esr & (1u << 6) ? FAULT_WRITE : FAULT_READ, f->elr, far);
    case EC_IABT_LOWER:
        if (area_fault_retry(far, MAP_EXEC))
            break;
        process_fault(FAULT_EXEC, f->elr, far);
    case EC_PC_ALIGN:
    case EC_SP_ALIGN:
        process_fault(FAULT_ALIGN, f->elr, far);
    case EC_BRK:
        process_fault(FAULT_BREAK, f->elr, f->elr);
    case EC_FP:
        if (aarch64_lazy_first_use(&task_self()->ctx))
            break;          /* run it again, now allowed */
        process_fault(FAULT_ILLEGAL, f->elr, f->elr);
    case EC_UNKNOWN:
    case EC_SYSREG:
    case EC_SVE:
        process_fault(FAULT_ILLEGAL, f->elr, f->elr);
    default:
        process_fault(FAULT_OTHER, f->elr, far);
    }
    user_return();          /* a killed task stops here */
    aarch64_lazy_return(&task_self()->ctx);
}

void trap_el0_irq(struct trap_frame *, uint64_t)
{
    irq_enter();
    irq_external();
    irq_exit();
    user_return();
    aarch64_lazy_return(&task_self()->ctx);
}

void trap_unexpected(struct trap_frame *f, uint64_t kind)
{
    guru(f, kind);
}

/* The frame is built on the current stack; once popped, SP_EL1 is where
 * the next exception from EL0 builds its frame. */
[[noreturn]] void arch_user_enter(uint64_t pc, uint64_t sp, uint64_t arg0, uint64_t arg1)
{
    struct trap_frame f = { 0 };

    f.x[0] = arg0;
    f.x[1] = arg1;
    f.sp = sp;
    f.elr = pc;
    f.spsr = 0;             /* EL0t, interrupts unmasked */
    arch_irq_disable();
    aarch64_lazy_return(&task_self()->ctx);
    trap_resume(&f);
}

bool arch_copy_from_user(void *dst, uint64_t src, size_t n)
{
    if (src >= ARCH_USER_TOP || n > ARCH_USER_TOP - src)
        return false;
    return probe_copy_from_user(dst, src, n) == 0;
}

bool arch_copy_to_user(uint64_t dst, const void *src, size_t n)
{
    if (dst >= ARCH_USER_TOP || n > ARCH_USER_TOP - dst)
        return false;
    return probe_copy_to_user(dst, src, n) == 0;
}

bool arch_probe_read(uint64_t addr, uint64_t *val)
{
    return probe_ld(addr, val) == 0;
}
