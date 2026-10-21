/*
 * servers/hwdb/drivers.c -- the match tables hwdb knows
 *
 * Until drivers are loaded from storage as ELF programs and carry their
 * tables in a .note.planar.match section (§11.6), the tables are here:
 * V4's drivers are all built into the image. Only the
 * drivers V4 plans are listed, each with the step that builds it: none
 * of them is a program yet, so a node "bound" to one says what will bind
 * it, in what order.
 *
 * The first three entries aren't processes. "kernel" is what the kernel
 * drives itself (kernel/devmem.c's registry refuses leases over them);
 * "firmware" is what M-mode owns on RISC-V; "hwdb" is what hwdb serves
 * itself: buses whose children the tree already lists, fixed clocks.
 */
#include "hwdb.h"

static const struct hw_match kernel[] = {
    { "riscv,cpu-intc",         HW_SPECIFIC },
    { "riscv,plic0",            HW_DEFAULT },
    { "sifive,plic-1.0.0",      HW_SPECIFIC },
    { "arm,gic-v3",             HW_SPECIFIC },
    { "arm,gic-v3-its",         HW_SPECIFIC },  /* MSIs, through AllocMsi */
    { "arm,armv8-timer",        HW_SPECIFIC },
    { "arm,psci-1.0",           HW_SPECIFIC },
    { "arm,psci-0.2",           HW_SPECIFIC },
    { "arm,sbsa-gwdt",          HW_SPECIFIC },  /* the kernel finds it in the GTDT */
    { "ky,x1-wdt",              HW_SPECIFIC },
    { "spacemit,k1x-wdt",       HW_SPECIFIC },
    { nullptr, 0 },
};

static const struct hw_match firmware[] = {
    { "riscv,clint0",           HW_DEFAULT },   /* the timer: through SBI */
    { "sifive,clint0",          HW_SPECIFIC },
    { "riscv,pmu",              HW_SPECIFIC },  /* SBI's PMU extension */
    { "sifive,test0",           HW_DEFAULT },   /* QEMU's reset: SBI's SRST */
    { nullptr, 0 },
};

static const struct hw_match hwdb[] = {
    { "simple-bus",             HW_GENERIC },
    { "fixed-clock",            HW_SPECIFIC },
    { "PNP0C02",                HW_SPECIFIC },  /* ACPI's reserved ranges: not a device */
    { nullptr, 0 },
};

/* Step 3: the first small drivers, to prove binding and restart. */
static const struct hw_match virtio_mmio[] = {
    { "virtio,mmio",            HW_SPECIFIC },
    { "LNRO0005",               HW_SPECIFIC },  /* QEMU's ACPI */
    { nullptr, 0 },
};

static const struct hw_match rtc_goldfish[] = {
    { "google,goldfish-rtc",    HW_SPECIFIC },
    { nullptr, 0 },
};

static const struct hw_match rtc_pl031[] = {
    { "arm,pl031",              HW_SPECIFIC },
    { "ARMH0031",               HW_SPECIFIC },
    { nullptr, 0 },
};

/* Step 5: storage. */
static const struct hw_match pci[] = {
    { "pci-host-ecam-generic",  HW_SPECIFIC },
    { "PNP0A08",                HW_DEFAULT },   /* PCI Express host bridge */
    { nullptr, 0 },
};

/* Functions pci finds behind a host bridge (drv/pci, servers/hwdb/pci.c). */
static const struct hw_match nvme[] = {
    { "pciclass,010802",        HW_GENERIC },   /* any NVMe controller */
    { nullptr, 0 },
};

static const struct hw_match sdhci[] = {
    { "ky,x1-sdhci",            HW_SPECIFIC },  /* the vendor's U-Boot tree */
    { "spacemit,k1-sdhci",      HW_SPECIFIC },  /* Linux's */
    { "pciclass,080501",        HW_GENERIC },   /* any SDHCI with DMA (QEMU's sdhci-pci) */
    { nullptr, 0 },
};

/* Step 7: the framebuffer. */
static const struct hw_match display[] = {
    { "simple-framebuffer",     HW_GENERIC },
    { nullptr, 0 },
};

/* V5 step 2: power (drv/power): the K1's power domains and the clock and
 * reset controllers that share their registers; the CIX's SCMI mailbox 6
 * and its shared memory; and the model hwdb's test mode adds on QEMU. */
static const struct hw_match power[] = {
    { "ky,x1-pm-domain",        HW_SPECIFIC },
    { "ky,x1-ccu",              HW_SPECIFIC },
    { "ky,x1-reset",            HW_SPECIFIC },
    { "CIXHA004",               HW_SPECIFIC },  /* SCMI's shared memory, to the SCP */
    { "planar,power-model",     HW_SPECIFIC },
    { nullptr, 0 },
};

const struct hw_driver hw_drivers[] = {
    { "kernel",         HW_DRV_KERNEL,      kernel },
    { "firmware",       HW_DRV_FIRMWARE,    firmware },
    { "hwdb",           HW_DRV_HWDB,        hwdb },
    { "virtio-mmio",    HW_DRV_PROCESS,     virtio_mmio },
    { "rtc-goldfish",   HW_DRV_PROCESS,     rtc_goldfish },
    { "rtc-pl031",      HW_DRV_PROCESS,     rtc_pl031 },
    { "pci",            HW_DRV_PROCESS,     pci },
    { "nvme",           HW_DRV_PROCESS,     nvme },
    { "sdhci",          HW_DRV_PROCESS,     sdhci },
    { "display",        HW_DRV_PROCESS,     display },
    { "power",          HW_DRV_PROCESS,     power },
};

const unsigned hw_ndrivers = sizeof hw_drivers / sizeof hw_drivers[0];
