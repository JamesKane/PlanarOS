/*
 * drivers/plic.c -- RISC-V platform-level interrupt controller
 *
 * Register layout from the RISC-V PLIC specification. Interrupt level n
 * is PLIC priority n; claim and complete give "masked from delivery until
 * acknowledged" directly. From omega's drivers/plic.c, with the contexts
 * read from the device tree instead of assumed.
 */
#include <planar/arch_inline.h>
#include <planar/endian.h>

#include "cpu.h"
#include "devmem.h"
#include "fdt.h"
#include "plic.h"

#define PRIORITY(src)       (4 * (uint64_t)(src))
#define ENABLE(ctx, src)    (0x2000 + 0x80 * (uint64_t)(ctx) + 4 * ((uint64_t)(src) / 32))
#define THRESHOLD(ctx)      (0x200000 + 0x1000 * (uint64_t)(ctx))
#define CLAIM(ctx)          (THRESHOLD(ctx) + 4)

constexpr uint32_t IRQ_S_EXTERNAL = 9;

static uintptr_t base;
static int32_t plic_node = -1;
static uint32_t nsources;
static uint32_t contexts[MAX_CPUS];     /* each CPU's S-mode context */

#define context (contexts[this_cpu()->index])

static volatile uint32_t *reg(uint64_t offset)
{
    return (volatile uint32_t *)(base + offset);
}

static bool probe(int32_t node)
{
    uint64_t addr, size;

    if (!fdt_reg(node, 0, &addr, &size))
        return false;
    base = (uintptr_t)phys_to_virt(addr);
    devmem_claim(addr, size);
    plic_node = node;
    nsources = fdt_getprop_u32(node, "riscv,ndev", 0);
    return nsources > 0;
}

/*
 * The context for hart 'hart's S-mode external interrupts: the position,
 * in the PLIC's interrupts-extended list of (interrupt controller, cause)
 * pairs, of the pair naming that hart's controller and cause 9.
 */
static bool find_context(uint64_t hart, uint32_t *ctx)
{
    uint32_t len;
    const uint8_t *p = fdt_getprop(plic_node, "interrupts-extended", &len);

    for (uint32_t i = 0; p && (i + 1) * 8 <= len; i++) {
        int32_t intc = fdt_find_phandle(get_be32(p + 8 * i));
        int32_t cpu = intc >= 0 ? fdt_parent(intc) : -1;
        uint64_t id, size;

        if (get_be32(p + 8 * i + 4) == IRQ_S_EXTERNAL && cpu >= 0 &&
            fdt_reg(cpu, 0, &id, &size) && id == hart) {
            *ctx = i;
            return true;
        }
    }
    return false;
}

static bool init_cpu(uint64_t hart)
{
    if (!find_context(hart, &contexts[this_cpu()->index]))
        return false;
    for (uint32_t src = 0; src <= nsources; src += 32)
        *reg(ENABLE(context, src)) = 0;
    *reg(THRESHOLD(context)) = 0;
    return true;
}

static void set(uint32_t source, unsigned level)
{
    volatile uint32_t *enable = reg(ENABLE(context, source));
    uint32_t bit = 1u << (source % 32);

    if (source == 0 || source > nsources)
        return;
    *reg(PRIORITY(source)) = level;
    if (level)
        *enable |= bit;
    else
        *enable &= ~bit;
}

static uint32_t claim(void)
{
    return *reg(CLAIM(context));
}

static void complete(uint32_t source)
{
    *reg(CLAIM(context)) = source;
}

/* One cell per interrupt: the source number. */
static uint32_t fdt_irq(int32_t node, uint32_t index)
{
    uint32_t len;
    const uint8_t *p = fdt_getprop(node, "interrupts", &len);

    return p && (index + 1) * 4 <= len ? get_be32(p + 4 * index) : 0;
}

static const char *const compatible[] = { "riscv,plic0", "sifive,plic-1.0.0", nullptr };

const struct intc_driver plic_driver = {
    .name = "PLIC",
    .compatible = compatible,
    .probe = probe,
    .init_cpu = init_cpu,
    .set = set,
    .claim = claim,
    .complete = complete,
    .fdt_irq = fdt_irq,
};
