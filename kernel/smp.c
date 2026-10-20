/*
 * kernel/smp.c -- CPU enumeration, bring-up and cross-CPU calls
 */
#include "arch.h"
#include "cpu.h"
#include "fdt.h"
#include "intc.h"
#include "irq.h"
#include "kprintf.h"
#include "kstring.h"
#include "ktime.h"
#include "pmm.h"

struct cpu cpus[MAX_CPUS];
unsigned ncpus = 1;

constexpr size_t STACK_PAGES = 4;

[[noreturn]] void kmain_secondary(struct cpu *c);

static bool cpu_node_ok(int32_t node)
{
    uint32_t len;
    const char *type = fdt_getprop(node, "device_type", &len);
    const char *status = fdt_getprop(node, "status", &len);

    if (!type || !kstreq(type, "cpu"))
        return false;
    return !status || kstreq(status, "okay") || kstreq(status, "ok");
}

bool cpu_enumerate(uint64_t boot_hwid)
{
    int32_t list = fdt_path("/cpus");
    uint64_t hwid, size;

    cpus[0].index = 0;
    cpus[0].hwid = boot_hwid;
    cpus[0].node = -1;
    atomic_store(&cpus[0].online, true);
    ncpus = 1;
    if (list < 0)
        return false;
    for (int32_t n = fdt_child(list, -1); n >= 0; n = fdt_child(list, n)) {
        if (!cpu_node_ok(n) || !fdt_reg(n, 0, &hwid, &size))
            continue;
        hwid &= 0xffffff;               /* MPIDR: Aff2.Aff1.Aff0 */
        if (hwid == boot_hwid) {
            cpus[0].node = n;
            continue;
        }
        if (ncpus == MAX_CPUS) {
            kprintf("planar: more than %u CPUs; the rest stay off\n", MAX_CPUS);
            break;
        }
        cpus[ncpus] = (struct cpu){ .index = ncpus, .hwid = hwid, .node = n };
        ncpus++;
    }
    return cpus[0].node >= 0;
}

unsigned cpus_online(void)
{
    unsigned n = 0;

    for (unsigned i = 0; i < ncpus; i++)
        n += atomic_load(&cpus[i].online);
    return n;
}

unsigned smp_start(void)
{
    for (unsigned i = 1; i < ncpus; i++) {
        struct cpu *c = &cpus[i];
        uint64_t stack = pmm_alloc_contig(STACK_PAGES), start;

        if (!stack) {
            kprintf("planar: no stack for cpu %u\n", i);
            continue;
        }
        c->stack_top = (uintptr_t)phys_to_virt(stack + STACK_PAGES * PAGE_SIZE);
        if (!arch_cpu_start(c)) {
            kprintf("planar: cpu %u (%llx) didn't start\n", i, (unsigned long long)c->hwid);
            continue;
        }
        /* Wait up to a second for it to say it's up. */
        start = time_now();
        while (!atomic_load(&c->online) && time_now() - start < time_hz())
            arch_cpu_relax();
        if (!atomic_load(&c->online))
            kprintf("planar: cpu %u (%llx) started but never came online\n", i,
                    (unsigned long long)c->hwid);
    }
    return cpus_online();
}

/* Each secondary CPU starts here, on its own stack, with this_cpu() set. */
[[noreturn]] void kmain_secondary(struct cpu *c)
{
    arch_cpu_init();
    if (!intc_init_cpu(c->hwid))
        kprintf("planar: cpu %u has no interrupt controller\n", c->index);
    ipi_init_cpu();
    time_init_cpu();
    arch_irq_enable();
    atomic_store(&c->online, true);
    for (;;)
        arch_wait_for_interrupt();
}

bool cpu_call(struct cpu *c, struct cpu_call *call, bool wait)
{
    atomic_store(&call->done, false);
    if (c == this_cpu()) {
        unsigned long was = arch_irq_save();

        call->fn(call->arg);
        atomic_store(&call->done, true);
        arch_irq_restore(was);
        return true;
    }
    if (!atomic_load(&c->online))
        return false;
    unsigned long was = spin_lock_irqsave(&c->call_lock);
    call->next = c->calls;
    c->calls = call;
    spin_unlock_irqrestore(&c->call_lock, was);
    arch_send_ipi(c);
    while (wait && !atomic_load_explicit(&call->done, memory_order_acquire))
        arch_cpu_relax();
    return true;
}

void ipi_handle(void)
{
    struct cpu *self = this_cpu();
    struct cpu_call *list;

    atomic_fetch_add(&self->ipis, 1);
    spin_lock(&self->call_lock);
    list = self->calls;
    self->calls = nullptr;
    spin_unlock(&self->call_lock);
    while (list) {
        struct cpu_call *next = list->next;

        list->fn(list->arg);
        atomic_store_explicit(&list->done, true, memory_order_release);
        list = next;
    }
}

static void ipi_source(void *)
{
    ipi_handle();
}

/* Take IPIs on this CPU. Where they come through the interrupt
 * controller (GIC SGIs), they're per-CPU sources enabled one CPU at a
 * time; on RISC-V they have a trap cause of their own. */
void ipi_init_cpu(void)
{
    uint32_t source = arch_ipi_irq();

    if (source)
        irq_attach(source, 6, ipi_source, nullptr);
}
