/*
 * kernel/irq.c -- interrupt dispatch
 */
#include "cpu.h"
#include "intc.h"
#include "irq.h"
#include "kprintf.h"
#include "ktime.h"

static struct {
    irq_handler_t fn;
    void *data;
} handlers[IRQ_SOURCES];

static uint64_t taken;

bool irq_attach(uint32_t source, unsigned level, irq_handler_t fn, void *data)
{
    if (source == 0 || source >= IRQ_SOURCES || level < 1 || level > 7 || !intc())
        return false;
    handlers[source].fn = fn;
    handlers[source].data = data;
    intc_set(source, level);
    return true;
}

void irq_external(void)
{
    uint32_t source;

    while ((source = intc_claim()) != 0) {
        taken++;
        if (source < IRQ_SOURCES && handlers[source].fn) {
            handlers[source].fn(handlers[source].data);
        } else {
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
