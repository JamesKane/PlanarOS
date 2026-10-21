/*
 * kernel/acpi.h -- the static ACPI tables (docs §9.4, layer 0)
 *
 * The second source of what the kernel reads before any server runs:
 * the same facts fdt.c gives, from an Arm ACPI board's tables (V3,
 * docs/v3-plan.md). Static tables only: the kernel never runs AML (VQ7);
 * the namespace is acpid's.
 *
 * The firmware wrote the tables, but a bad one should make a missing
 * device, not a hang or a stray access: every table's length and
 * checksum are checked, every structure in one must lie inside it, and
 * every walk moves forward (as fdt.c, review M14).
 */
#ifndef KERNEL_ACPI_H
#define KERNEL_ACPI_H

#include <stddef.h>
#include <stdint.h>

constexpr unsigned ACPI_MAX_GICR = 8;
constexpr unsigned ACPI_MAX_ITS = 8;

/* Read the tables from the RSDP at 'rsdp' (mapped): the XSDT and every
 * table it lists whose length and checksum are good. False if the RSDP
 * or the XSDT isn't good; then no table is found. */
bool acpi_init(const void *rsdp);

/* Table 'sig' ("APIC", "GTDT"...), and its length; nullptr if there's
 * none. The first, if the XSDT lists more than one. */
const void *acpi_table(const char *sig, uint32_t *len);

/* Every blob of firmware data in the tables, by index from 0: the RSDP,
 * the XSDT, each table it lists, and the FADT's DSDT and FACS. False
 * past the last. For handing them to user mode (MapFirmware). */
bool acpi_blob(unsigned index, uint64_t *pa, uint64_t *len);

/* The GIC, from the MADT. Redistributors are GICR ranges, or one per
 * enabled CPU (size 0: one redistributor) where the MADT gives them in
 * its GICC entries instead. The ITSs are recorded for V4. */
struct acpi_range {
    uint64_t base, size;
};
struct acpi_gic {
    uint64_t gicd;
    uint32_t version;               /* the GICD entry's: 3, 4; 0 if not given */
    struct acpi_range gicr[ACPI_MAX_GICR];
    unsigned ngicr;
    uint64_t its[ACPI_MAX_ITS];
    unsigned nits;
};
bool acpi_gic(struct acpi_gic *g);

/* GICC entry 'index' of the MADT: a CPU. False past the end. */
struct acpi_cpu {
    uint64_t mpidr;                 /* Aff3, Aff2.Aff1.Aff0 */
    uint32_t uid;
    bool enabled;                   /* usable now (else maybe online-capable) */
    uint64_t gicr;                  /* its redistributor; 0 if from a GICR range */
    uint8_t efficiency;             /* the entry's efficiency class; 0 if none */
};
bool acpi_cpu(unsigned index, struct acpi_cpu *c);

/* ACPI processor 'uid''s cluster, from the PPTT: an opaque ID that its
 * cluster's CPUs share (the node's offset). False if there's no PPTT, or
 * it doesn't place the CPU. */
bool acpi_cluster(uint32_t uid, uint32_t *cluster);

/* The generic timer's interrupts, from the GTDT: 0 where none is given. */
struct acpi_timer {
    uint32_t el1_phys, el1_virt, el2_phys;
};
bool acpi_timer(struct acpi_timer *t);

/* SBSA watchdog 'index' of the GTDT; false past the end. */
struct acpi_watchdog {
    uint64_t refresh, control;      /* the frames */
    uint32_t gsiv;
};
bool acpi_watchdog(unsigned index, struct acpi_watchdog *w);

/* The console: the SPCR's port, or, on a board whose quirk says so, the
 * DBG2's first serial port. */
enum acpi_uart { ACPI_UART_PL011 = 1, ACPI_UART_16550 };
struct acpi_console {
    enum acpi_uart kind;
    uint64_t base;
    uint8_t reg_shift, reg_width;   /* 16550: register spacing and width */
    const char *table;              /* "SPCR" or "DBG2" */
};
bool acpi_console(struct acpi_console *c);

/* PSCI, from the FADT: false if the firmware doesn't implement it; else
 * whether it's called by SMC (else HVC). */
bool acpi_psci(bool *smc);

/* PCI configuration space, MCFG entry 'index' (recorded for V4). */
struct acpi_pci {
    uint64_t base;
    uint16_t segment;
    uint8_t bus_start, bus_end;
};
bool acpi_pci(unsigned index, struct acpi_pci *p);

/* Reserved range 'index' of the board's quirks: memory the firmware
 * hands out as free but keeps (the CIX's RSVL carve-outs). */
bool acpi_reserved(unsigned index, uint64_t *base, uint64_t *size);

/* Shared range 'index': memory the firmware and a driver both use (the
 * CIX's SMC channel for SCMI). Reserved, like the above, and the one
 * memory but a framebuffer that a lease may cover (kernel/lease.c). */
bool acpi_shared(unsigned index, uint64_t *base, uint64_t *size);

/* The board's quirk entry's name, or nullptr if it has none. */
const char *acpi_quirk_name(void);

/* Print the tables found and the facts read from them. */
void acpi_report(void);
void acpi_dump_tables(void);   /* "dumpacpi": every table in hex */

#endif /* KERNEL_ACPI_H */
