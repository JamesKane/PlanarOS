/*
 * plat/qemu-virt/plat.c -- QEMU's 'virt' machine, riscv64 and aarch64
 *
 * One plat/ for both architectures: everything V0 needs comes from the
 * device tree QEMU generates, so nothing here depends on which one it is
 * but RV64's ramfb, which edk2 sets up on AArch64.
 */
#include "arch.h"
#include "bootinfo.h"
#include "console.h"
#include "fdt.h"
#include "kstring.h"
#include "plat.h"

const char plat_name[] = "qemu-virt";

void plat_early_init(void)
{
    /* The firmware (OpenSBI, or QEMU itself) has already set up the UART. */
    console_init();
}

/* The device tree always names the console; there is no fallback. */
void plat_console_putc(char c)
{
    (void)c;
}

void plat_late_init(void)
{
}

/* The firmware's reset works here (SBI SRST, PSCI). */
void plat_reboot(void)
{
}

/*
 * QEMU's ramfb (V7 step 1): a framebuffer in guest RAM that the guest
 * describes to QEMU through fw_cfg's "etc/ramfb". On AArch64 edk2 does
 * that and the stub finds a GOP; on RV64 nothing does, so the kernel
 * stands in for the firmware here, once, and the framebuffer is then the
 * board's like the K1's: kept from the allocator, leased to display.
 *
 * RV64 only: its direct map is every physical address, devices included,
 * where AArch64's maps RAM alone, and AArch64's screens come from edk2.
 */
constexpr uint32_t RAMFB_W = 1280, RAMFB_H = 800;
constexpr uint64_t RAMFB_ALIGN = 2ULL << 20;

/* fw_cfg's MMIO registers; the selector and DMA address are big-endian. */
constexpr uint64_t FWCFG_SEL = 0x08, FWCFG_DMA = 0x10;
constexpr uint16_t FWCFG_ID = 0x01, FWCFG_FILE_DIR = 0x19;
constexpr uint32_t FWCFG_VERSION_DMA = 1 << 1;
constexpr uint32_t DMA_ERROR = 1, DMA_READ = 2, DMA_SELECT = 8, DMA_WRITE = 0x10;

struct fwcfg_dma {
    uint32_t control, length;
    uint64_t address;
} __attribute__((aligned(16)));

struct fwcfg_file {
    uint32_t size;
    uint16_t select, reserved;
    char name[56];
};

/* What QEMU reads from "etc/ramfb", packed and big-endian. */
struct ramfb_cfg {
    uint64_t addr;
    uint32_t fourcc, flags, width, height, stride;
} __attribute__((packed));

static uint64_t fwcfg;

/* One DMA transfer: QEMU does it before the store returns. */
static bool fwcfg_dma(uint32_t control, void *buf, uint32_t len)
{
    static struct fwcfg_dma d;

    d = (struct fwcfg_dma){ __builtin_bswap32(control), __builtin_bswap32(len),
                            __builtin_bswap64(virt_to_phys(buf)) };
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    *(volatile uint64_t *)phys_to_virt(fwcfg + FWCFG_DMA) = __builtin_bswap64(virt_to_phys(&d));
    while (__builtin_bswap32(*(volatile uint32_t *)&d.control) & ~DMA_ERROR)
        ;
    return !(__builtin_bswap32(d.control) & DMA_ERROR);
}

static bool fwcfg_read(int sel, void *buf, uint32_t len)
{
    return fwcfg_dma((sel >= 0 ? (uint32_t)sel << 16 | DMA_SELECT : 0) | DMA_READ, buf, len);
}

/* The item named 'name', or 0. */
static uint16_t fwcfg_find(const char *name)
{
    struct fwcfg_file f;
    uint32_t n;

    if (!fwcfg_read(FWCFG_FILE_DIR, &n, sizeof n))
        return 0;
    for (n = __builtin_bswap32(n); n; n--) {
        if (!fwcfg_read(-1, &f, sizeof f))
            return 0;
        if (memcmp(f.name, name, kstrlen(name) + 1) == 0)
            return __builtin_bswap16(f.select);
    }
    return 0;
}

/* Under the device tree and the first bank's top, which QEMU's loader
 * leaves free: it puts the tree at the end of RAM (below 3 GiB). */
static uint64_t ramfb_place(uint64_t size)
{
    int32_t node = -1;
    uint64_t base, bank, top = virt_to_phys(fdt_blob());

    while ((node = fdt_find_name(node, "memory")) >= 0)
        if (fdt_reg(node, 0, &base, &bank) && base < top && top <= base + bank)
            return ((top & ~(RAMFB_ALIGN - 1)) - size) & ~(RAMFB_ALIGN - 1);
    return 0;
}

bool plat_framebuffer(struct boot_fb *fb)
{
#if defined(__riscv)
    static struct ramfb_cfg cfg;
    uint64_t size = (uint64_t)RAMFB_W * RAMFB_H * 4, base, fsize;
    int32_t node = fdt_find_compatible(-1, "qemu,fw-cfg-mmio");
    uint32_t id = 0;
    uint16_t sel;

    if (node < 0 || !fdt_reg(node, 0, &fwcfg, &fsize))
        return false;
    /* The ID by the old interface: one byte at a time from the data port. */
    *(volatile uint16_t *)phys_to_virt(fwcfg + FWCFG_SEL) = __builtin_bswap16(FWCFG_ID);
    for (unsigned i = 0; i < 4; i++)
        id |= (uint32_t)*(volatile uint8_t *)phys_to_virt(fwcfg) << 8 * i;
    if (!(id & FWCFG_VERSION_DMA) || !(sel = fwcfg_find("etc/ramfb")) || !(base = ramfb_place(size)))
        return false;
    memset(phys_to_virt(base), 0, size);
    cfg = (struct ramfb_cfg){ __builtin_bswap64(base), __builtin_bswap32(0x34325258), 0,
                              __builtin_bswap32(RAMFB_W), __builtin_bswap32(RAMFB_H),
                              __builtin_bswap32(RAMFB_W * 4) };     /* XR24 */
    if (!fwcfg_dma((uint32_t)sel << 16 | DMA_SELECT | DMA_WRITE, &cfg, sizeof cfg))
        return false;
    /* XRGB8888 is B, G, R, X in memory: GOP's format 1. */
    *fb = (struct boot_fb){ .base = base, .size = size, .width = RAMFB_W, .height = RAMFB_H,
                            .stride = RAMFB_W, .format = 1 };
    return true;
#else
    (void)fb;
    return false;
#endif
}
