/*
 * kernel/intc.h -- the interrupt controller, from the firmware description
 *
 * The kernel finds its interrupt controller before any server runs
 * (docs §9.4, layer 0). Each architecture lists the drivers it can have
 * (arch_intc_drivers); intc_init() picks the first whose compatible
 * string the device tree has, or, booted from ACPI, the first that finds
 * its controller in the tables. Sources are the controller's own
 * numbers: PLIC source IDs, GIC INTIDs.
 */
#ifndef KERNEL_INTC_H
#define KERNEL_INTC_H

#include <stdint.h>

struct intc_driver {
    const char *name;
    const char *const *compatible;      /* nullptr-terminated */

    /* Set up the controller described by device-tree node 'node'. */
    bool (*probe)(int32_t node);

    /* Set up the controller the ACPI tables describe; false if they
     * describe none of this kind. nullptr if the driver can't. */
    bool (*probe_acpi)(void);

    /* Set up the calling CPU's side; 'hwid' is its hart ID or MPIDR
     * affinity. Drivers keep per-CPU state by this_cpu()->index. */
    bool (*init_cpu)(uint64_t hwid);

    /* Deliver 'source' to the calling CPU at priority level 1-7 (Exec's
     * interrupt levels), or mask it there with 0. Per-CPU sources (GIC
     * SGIs and PPIs) are enabled CPU by CPU; shared ones go to one CPU. */
    void (*set)(uint32_t source, unsigned level);

    /* Take the highest-priority pending source (0 if none); it stays
     * masked until complete(). */
    uint32_t (*claim)(void);
    void (*complete)(uint32_t source);

    /* Entry 'index' of a device-tree node's interrupts property, as a
     * source number; 0 if there isn't one. */
    uint32_t (*fdt_irq)(int32_t node, uint32_t index);

    /* Print where the controller is, after its name, for hw_report()'s
     * line (kernel/hw.h); nullptr to print the name alone. */
    void (*describe)(void);
};

/* Find and set up the controller, and the boot CPU's side of it. */
bool intc_init(uint64_t boot_cpu);

/* Set up the calling CPU's side, on a CPU started later. */
bool intc_init_cpu(uint64_t hwid);

/* The driver in use, or nullptr before intc_init(). */
const struct intc_driver *intc(void);

void intc_set(uint32_t source, unsigned level);
uint32_t intc_claim(void);
void intc_complete(uint32_t source);
uint32_t intc_fdt_irq(int32_t node, uint32_t index);

#endif /* KERNEL_INTC_H */
