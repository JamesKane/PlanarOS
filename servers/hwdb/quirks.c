/*
 * servers/hwdb/quirks.c -- board quirks, as data (§9.4 Layer 2, D5 (a))
 *
 * Each entry names its board and a node by path. The early quirks the
 * kernel needs itself (its console, reserved memory) are kernel/acpi.c's;
 * an entry here may repeat one, since hwdb reads the tables again.
 */
#include "hwdb.h"

const struct hw_quirk hw_quirk_table[] = {
    /*
     * The K1 under its vendor U-Boot (the root says only "ky,x1"). Its
     * clock and reset controllers and its power domains each list the
     * same eight register blocks, which the kernel's watchdog, the DDR
     * controller and PCIe share. Everything `power` drives in V5 is in
     * one of them, the APMU (drv/power/k1.c): power is leased its page
     * alone, and the other blocks stay as U-Boot set them. PCIe's and
     * the USB3/PCIe combo PHY's words in that page are PHY controls that
     * power will drive for them when they have drivers (step 4).
     */
    { "ky,x1", HW_Q_REGS, "/soc/clock-controller@d4050000", "0xd4282800" },
    { "ky,x1", HW_Q_REGS, "/soc/reset-controller@d4050000", "0xd4282800" },
    { "ky,x1", HW_Q_REGS, "/soc/power-management@0", "0xd4282800" },
    { "ky,x1", HW_Q_REGS, "/soc/phy@c0b10000", "0xc0b10000" },
    { "ky,x1", HW_Q_REGS, "/soc/pcie@ca400000",
      "0xca400000 0xca700000 0x90000000 0xc0c20000 0xc0c10000 0xc0b10000" },
    { "ky,x1", HW_Q_FIRMWARE, "/soc/pinctrl@d401e000", "U-Boot's pins" },
    /* Its GPIOs: the SD slot's card detect is one, which sdhci does
     * without (it asks the card). */
    { "ky,x1", HW_Q_FIRMWARE, "/soc/gpio@d4019000", "U-Boot's, until a GPIO driver" },
    /* The display sits behind dram_range1 (Linux's k1.dtsi), where memory
     * above 4 GiB is seen 2 GiB lower and only the first 2 GiB where it
     * is; U-Boot's tree has no dram_range buses to say so. */
    { "ky,x1", HW_Q_DMA_LIMIT, "/soc/dpu@c0340000", "0x80000000" },

    /*
     * The CIX's SCMI (drv/power/scmi.c): `power` drives mailbox 6 and its
     * shared memory, SHM0, and through SHM0's lease TF-A's channel, which
     * only the device tree names (kernel/acpi.c lets it be leased), rung
     * by an SMC. The mailboxes all say CIXHA001, so MBX6 is named here;
     * mailbox 7 is the SCP's way back, for interrupts, which power doesn't
     * use.
     */
    { "CIXTEK SKY1EDK2", HW_Q_PIN, "/_SB_/MBX6", "power" },
    { "CIXTEK SKY1EDK2", HW_Q_MMIO, "/_SB_/SHM0", "0x84380000 0x80" },
    { "CIXTEK SKY1EDK2", HW_Q_CALL, "/_SB_/SHM0", "0xc2000001" },

    /* The CIX P1 has no SPCR. The kernel takes DBG2's port as its
     * console (kernel/acpi.c's quirk), so the node is the kernel's. */
    { "CIXTEK SKY1EDK2", HW_Q_CONSOLE, "/DBG2/serial@40d0000", nullptr },

    /*
     * Each PCIe root complex is described twice: as the host bridge
     * (PNP0A08, with the MCFG's ECAM) and as CIXH2020, whose _CRS lists
     * the same ECAM and the first MiB of the bridge's window, where the
     * firmware put the NVMe's BAR. The firmware trains the links, and
     * nothing drives CIXH2020 in V4: its registers are the bridge's.
     */
    { "CIXTEK SKY1EDK2", HW_Q_ALIAS, "/_SB_/PRC0", "/_SB_/PCI0" },
    { "CIXTEK SKY1EDK2", HW_Q_ALIAS, "/_SB_/PRC1", "/_SB_/PCI1" },
    { "CIXTEK SKY1EDK2", HW_Q_ALIAS, "/_SB_/PRC2", "/_SB_/PCI2" },
    { "CIXTEK SKY1EDK2", HW_Q_ALIAS, "/_SB_/PRC3", "/_SB_/PCI3" },
    { "CIXTEK SKY1EDK2", HW_Q_ALIAS, "/_SB_/PRC4", "/_SB_/PCI4" },
};

const unsigned hw_nquirks = sizeof hw_quirk_table / sizeof hw_quirk_table[0];

/* The next number in a quirk's list ("0x80000000 0x1000"), and past it. */
static uint64_t hex(const char **s)
{
    const char *c = *s;
    uint64_t v = 0;

    while (*c == ' ')
        c++;
    if (c[0] == '0' && (c[1] | 0x20) == 'x')
        c += 2;
    for (; (*c >= '0' && *c <= '9') || ((*c | 0x20) >= 'a' && (*c | 0x20) <= 'f'); c++)
        v = v << 4 | (uint64_t)(*c <= '9' ? *c - '0' : (*c | 0x20) - 'a' + 10);
    while (*c == ' ')
        c++;
    *s = c;
    return v;
}

static void disable(struct hw_graph *g, hw_id id)
{
    g->nodes[id].enabled = false;
    for (hw_id i = id + 1; i < g->count; i++)
        if (g->nodes[i].parent != HW_NONE && !g->nodes[g->nodes[i].parent].enabled)
            g->nodes[i].enabled = false;
}

unsigned hw_quirks(struct hw_graph *g, const struct hw_quirk *q, unsigned n)
{
    unsigned applied = 0;

    for (unsigned i = 0; i < n; i++, q++) {
        hw_id id;

        if (!g->board || !hw_streq(q->board, g->board))
            continue;
        id = hw_find_path(g, q->path, hw_strlen(q->path));
        if (id == HW_NONE)
            id = hw_find_moved(g, q->path);
        if (id == HW_NONE && q->kind == HW_Q_ALIAS)
            continue;           /* one of the two descriptions is missing: nothing to say */
        if (id == HW_NONE && !g->ns_first && hw_strneq(q->path, "/_SB_/", 6))
            continue;           /* acpid's device, and acpid didn't run (or failed) */
        if (id == HW_NONE) {
            g->broken++;
            continue;
        }
        switch (q->kind) {
        case HW_Q_DISABLE:
            disable(g, id);
            break;
        case HW_Q_FIRMWARE:
            g->nodes[id].state = HW_S_FIRMWARE;
            g->nodes[id].why = q->arg;
            break;
        case HW_Q_CONSOLE:
            g->console = id;
            break;
        case HW_Q_PIN:
            g->nodes[id].pin = q->arg;
            break;
        case HW_Q_EXCLUDE:
            g->nodes[id].exclude = q->arg;
            break;
        case HW_Q_DMA_LIMIT: {
            const char *c = q->arg;

            g->nodes[id].dma_limit = hex(&c);
            break;
        }
        case HW_Q_CALL: {
            const char *c = q->arg;

            g->nodes[id].call = (uint32_t)hex(&c);
            break;
        }
        case HW_Q_REGS: {
            struct hw_node *d = &g->nodes[id];
            unsigned kept = 0;

            for (unsigned k = 0; k < d->nres; k++) {
                const char *c = q->arg;
                bool keep = d->res[k].kind != HW_MMIO;

                while (*c && !keep)
                    keep = hex(&c) == d->res[k].base;
                if (keep)
                    d->res[kept++] = d->res[k];
            }
            d->nres = (uint16_t)kept;
            break;
        }
        case HW_Q_MMIO: {
            const char *c = q->arg;
            uint64_t base = hex(&c), size = hex(&c);
            struct hw_res *r = hw_res_add(g, id);

            if (!r) {
                g->broken++;
                continue;
            }
            *r = (struct hw_res){ .kind = HW_MMIO, .ctlr = HW_NONE, .base = base, .size = size };
            break;
        }
        case HW_Q_ALIAS: {
            hw_id to = hw_find_path(g, q->arg, hw_strlen(q->arg));

            if (to == HW_NONE)
                to = hw_find_moved(g, q->arg);
            if (to == HW_NONE)
                continue;
            g->nodes[id].alias = to;
            break;
        }
        }
        applied++;
    }
    return applied;
}
