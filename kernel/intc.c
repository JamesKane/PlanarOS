/*
 * kernel/intc.c -- the interrupt controller, from the firmware description
 */
#include "arch.h"
#include "fdt.h"
#include "intc.h"
#include "kprintf.h"

static const struct intc_driver *drv;

bool intc_init(uint64_t boot_cpu)
{
    for (const struct intc_driver *const *d = arch_intc_drivers; *d; d++) {
        for (const char *const *c = (*d)->compatible; *c; c++) {
            int32_t node = fdt_find_compatible(-1, *c);

            if (node < 0)
                continue;
            if (!(*d)->probe(node) || !(*d)->init_cpu(boot_cpu)) {
                kprintf("planar: %s at %s didn't come up\n", (*d)->name,
                        fdt_node_name(node));
                return false;
            }
            drv = *d;
            return true;
        }
    }
    return false;
}

const struct intc_driver *intc(void)
{
    return drv;
}

void intc_set(uint32_t source, unsigned level)
{
    if (drv)
        drv->set(source, level);
}

uint32_t intc_claim(void)
{
    return drv ? drv->claim() : 0;
}

void intc_complete(uint32_t source)
{
    if (drv)
        drv->complete(source);
}

uint32_t intc_fdt_irq(int32_t node, uint32_t index)
{
    if (drv)
        return drv->fdt_irq(node, index);
    /* Before intc_init(), ask the driver whose controller is described. */
    for (const struct intc_driver *const *d = arch_intc_drivers; *d; d++)
        for (const char *const *c = (*d)->compatible; *c; c++)
            if (fdt_find_compatible(-1, *c) >= 0)
                return (*d)->fdt_irq(node, index);
    return 0;
}
