/*
 * kernel/irq.c -- interrupt dispatch
 */
#include "cpu.h"
#include "intc.h"
#include "irq.h"
#include "kprintf.h"
#include "ktime.h"

static struct {
    _Atomic irq_handler_t fn;
    void *data;
    atomic_bool running;        /* its handler runs now, on some CPU */
} handlers[IRQ_SOURCES];

static uint64_t taken;

bool irq_attach(uint32_t source, unsigned level, irq_handler_t fn, void *data)
{
    if (source == 0 || source >= IRQ_SOURCES || level < 1 || level > 7 || !intc())
        return false;
    handlers[source].data = data;
    atomic_store(&handlers[source].fn, fn);    /* after 'data': see irq_detach */
    intc_set(source, level);
    return true;
}

/*
 * A handler marks itself running before it reads 'fn', and this clears
 * 'fn' before it looks: so either the handler sees nullptr, or this
 * waits for it to finish.
 */
void irq_detach(uint32_t source)
{
    if (source == 0 || source >= IRQ_SOURCES)
        return;
    intc_set(source, 0);
    atomic_store(&handlers[source].fn, nullptr);
    while (atomic_load(&handlers[source].running))
        arch_cpu_relax();
    handlers[source].data = nullptr;
}

bool irq_attached(uint32_t source)
{
    return source < IRQ_SOURCES && handlers[source].fn;
}

void irq_external(void)
{
    uint32_t source;

    while ((source = intc_claim()) != 0) {
        taken++;
        irq_handler_t fn = nullptr;

        if (source < IRQ_SOURCES) {
            atomic_store(&handlers[source].running, true);
            fn = atomic_load(&handlers[source].fn);
            if (fn)
                fn(handlers[source].data);
            atomic_store(&handlers[source].running, false);
        }
        if (!fn) {
            /* Nobody wants it: mask it so it can't storm. */
            kprintf("planar: unexpected interrupt %u, masked\n", source);
            intc_set(source, 0);
        }
        intc_complete(source);
    }
}

void irq_timer(void)
{
    taken++;
    time_expired();
}

void irq_ipi(void)
{
    taken++;
    ipi_handle();
}

uint64_t irq_count(void)
{
    return taken;
}
