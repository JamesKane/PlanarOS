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
     * power will drive for them when they have drivers (step 4). The
     * combo PHY is USB's (below): PCIe0 isn't driven, and port A's
     * calibration by a PCIe driver would reset it.
     */
    { "ky,x1", HW_Q_REGS, "/soc/clock-controller@d4050000", "0xd4282800" },
    { "ky,x1", HW_Q_REGS, "/soc/reset-controller@d4050000", "0xd4282800" },
    { "ky,x1", HW_Q_REGS, "/soc/power-management@0", "0xd4282800" },
    { "ky,x1", HW_Q_REGS, "/soc/pcie@ca400000",
      "0xca400000 0xca700000 0x90000000 0xc0c20000 0xc0c10000" },
    { "ky,x1", HW_Q_FIRMWARE, "/soc/pinctrl@d401e000", "U-Boot's pins" },
    /* Its GPIOs are power's, for the supplies that are GPIOs (the USB
     * hub's VBUS): leased with the APMU. The SD slot's card detect is one
     * too, which sdhci does without (it asks the card). */
    { "ky,x1", HW_Q_MMIO, "/soc/clock-controller@d4050000", "0xd4019000 0x800" },
    { "ky,x1", HW_Q_ALIAS, "/soc/gpio@d4019000", "/soc/clock-controller@d4050000" },
    /* The display sits behind dram_range1 (Linux's k1.dtsi), where memory
     * above 4 GiB is seen 2 GiB lower and only the first 2 GiB where it
     * is; U-Boot's tree has no dram_range buses to say so. */
    { "ky,x1", HW_Q_DMA_LIMIT, "/soc/dpu@c0340000", "0x80000000" },
    /* One node for two DPUs (reg-names "dsi", "hdmi"): display drives
     * the HDMI one, which U-Boot lit, on its online interrupt (the
     * vendor's x1-hdmi.dtsi; U-Boot's tree lists none), and draws its
     * console on U-Boot's framebuffer. */
    { "ky,x1", HW_Q_REGS, "/soc/dpu@c0340000", "0xc0440000" },
    { "ky,x1", HW_Q_IRQ, "/soc/dpu@c0340000", "0x8b /soc/interrupt-controller@e0000000" },
    { "ky,x1", HW_Q_SCANOUT, "/soc/dpu@c0340000", nullptr },
    /* The USB-A ports' DWC3 (drv/xhci): its glue node holds the clock
     * and reset power votes for, and xhci drives it with its child's
     * registers and line, and its PHYs (the vendor's dwc3-ky.c); its own
     * line is a wakeup's. The USB 3 PHY's mode and reset, and the hub's
     * VBUS, are power's: the set "usb", which xhci votes for. */
    { "ky,x1", HW_Q_MMIO, "/soc/usb3@0", "0xc0a00000 0x10000" },
    { "ky,x1", HW_Q_MMIO, "/soc/usb3@0", "0xc0a30000 0x200" },
    { "ky,x1", HW_Q_MMIO, "/soc/usb3@0", "0xc0b10000 0x800" },
    { "ky,x1", HW_Q_NOIRQ, "/soc/usb3@0", "0x95 /soc/interrupt-controller@e0000000" },
    { "ky,x1", HW_Q_IRQ, "/soc/usb3@0", "0x7d /soc/interrupt-controller@e0000000" },
    { "ky,x1", HW_Q_ALIAS, "/soc/usb3@0/dwc3@c0a00000", "/soc/usb3@0" },
    { "ky,x1", HW_Q_ALIAS, "/soc/usb2phy@0xc0a30000", "/soc/usb3@0" },
    { "ky,x1", HW_Q_ALIAS, "/soc/phy@c0b10000", "/soc/usb3@0" },
    { "ky,x1", HW_Q_DMA_LIMIT, "/soc/usb3@0", "0x80000000" },     /* dram_range0 */
    /* Its hub's power and VBUS: GPIO 123 (Linux's tree; U-Boot's says 79),
     * which power drives in the set "usb". */
    { "ky,x1", HW_Q_FIRMWARE, "/usb3hub", "power's, in the set \"usb\"" },

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

    /*
     * The CIX's G720 (drv/gpu-panthor). Its power-on is GPUP's power
     * resource and memory repair, in its RCSU (its first window), and a
     * its resets in RST0's page, which also holds the DDR controller's,
     * the GIC's and the NoCs' resets, and the memory-repair strap (DMRP's
     * MVCK): the driver reads the strap, and sets its own two reset bits
     * there, read-modify-write, if they're held, and changes nothing else. The
     * three nodes that list that page (the power-domain controller, RST0
     * and CRU0) have no driver, so the page is taken from them.
     */
    { "CIXTEK SKY1EDK2", HW_Q_MMIO, "/_SB_/GPU_", "0x16000000 0x1000" },
    /* Its domain and clocks (SCMI's), which ACPI doesn't name: hwdb votes
     * for them before the lease, so they stay on until a dead driver's
     * GPU is stopped (stop_gpu touches its registers). */
    { "CIXTEK SKY1EDK2", HW_Q_POWER, "/_SB_/GPU_", "gpu" },
    { "CIXTEK SKY1EDK2", HW_Q_REGS, "/_SB_/PDC0", "0" },
    { "CIXTEK SKY1EDK2", HW_Q_REGS, "/_SB_/RST0", "0" },
    { "CIXTEK SKY1EDK2", HW_Q_REGS, "/_SB_/CRU0", "0" },

    /*
     * The CIX's monitor is on DPU4 (through DP04 and the PS185), where
     * the GOP left its framebuffer scaled to the mode it chose. The other
     * four DPUs are dark, and display doesn't wake them.
     */
    { "CIXTEK SKY1EDK2", HW_Q_SCANOUT, "/_SB_/DPU4", nullptr },
    { "CIXTEK SKY1EDK2", HW_Q_EXCLUDE, "/_SB_/DPU0", "display" },
    { "CIXTEK SKY1EDK2", HW_Q_EXCLUDE, "/_SB_/DPU1", "display" },
    { "CIXTEK SKY1EDK2", HW_Q_EXCLUDE, "/_SB_/DPU2", "display" },
    { "CIXTEK SKY1EDK2", HW_Q_EXCLUDE, "/_SB_/DPU3", "display" },

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

    /*
     * The CIX's ten USB controllers (Cadence's CDNSP, xhci), each
     * described twice: as PNP0D10, with its xHCI registers, and as
     * CIXH2031 under the vendor's CIXH2030, with its OTG and device
     * registers too and its wakeup line through PDC0. UEFI leaves them
     * all in host mode. Three PNP0D10s' _STA read GNVA, which acpid
     * can't (VQ42), and they come out disabled; the CIXH2031s' don't,
     * so xhci drives those, on their xHCI window and host line alone.
     * Their CIXH2030 parents are the wrappers' straps and resets, which
     * UEFI set.
     */
    { "CIXTEK SKY1EDK2", HW_Q_FIRMWARE, "/_SB_/SUB0", "UEFI's: host mode" },
    { "CIXTEK SKY1EDK2", HW_Q_REGS, "/_SB_/SUB0/CUB0", "0x9018000" },
    { "CIXTEK SKY1EDK2", HW_Q_NOIRQ, "/_SB_/SUB0/CUB0", "/_SB_/PDC0" },
    { "CIXTEK SKY1EDK2", HW_Q_ALIAS, "/_SB_/XHC0", "/_SB_/SUB0/CUB0" },
    { "CIXTEK SKY1EDK2", HW_Q_FIRMWARE, "/_SB_/SUB1", "UEFI's: host mode" },
    { "CIXTEK SKY1EDK2", HW_Q_REGS, "/_SB_/SUB1/CUB1", "0x9088000" },
    { "CIXTEK SKY1EDK2", HW_Q_NOIRQ, "/_SB_/SUB1/CUB1", "/_SB_/PDC0" },
    { "CIXTEK SKY1EDK2", HW_Q_ALIAS, "/_SB_/XHC1", "/_SB_/SUB1/CUB1" },
    { "CIXTEK SKY1EDK2", HW_Q_FIRMWARE, "/_SB_/SUB2", "UEFI's: host mode" },
    { "CIXTEK SKY1EDK2", HW_Q_REGS, "/_SB_/SUB2/CUB2", "0x90f8000" },
    { "CIXTEK SKY1EDK2", HW_Q_NOIRQ, "/_SB_/SUB2/CUB2", "/_SB_/PDC0" },
    { "CIXTEK SKY1EDK2", HW_Q_ALIAS, "/_SB_/XHC2", "/_SB_/SUB2/CUB2" },
    { "CIXTEK SKY1EDK2", HW_Q_FIRMWARE, "/_SB_/SUB3", "UEFI's: host mode" },
    { "CIXTEK SKY1EDK2", HW_Q_REGS, "/_SB_/SUB3/CUB3", "0x9168000" },
    { "CIXTEK SKY1EDK2", HW_Q_NOIRQ, "/_SB_/SUB3/CUB3", "/_SB_/PDC0" },
    { "CIXTEK SKY1EDK2", HW_Q_ALIAS, "/_SB_/XHC3", "/_SB_/SUB3/CUB3" },
    { "CIXTEK SKY1EDK2", HW_Q_FIRMWARE, "/_SB_/SUB4", "UEFI's: host mode" },
    { "CIXTEK SKY1EDK2", HW_Q_REGS, "/_SB_/SUB4/CUB4", "0x91d8000" },
    { "CIXTEK SKY1EDK2", HW_Q_NOIRQ, "/_SB_/SUB4/CUB4", "/_SB_/PDC0" },
    { "CIXTEK SKY1EDK2", HW_Q_ALIAS, "/_SB_/XHC4", "/_SB_/SUB4/CUB4" },
    { "CIXTEK SKY1EDK2", HW_Q_FIRMWARE, "/_SB_/SUB5", "UEFI's: host mode" },
    { "CIXTEK SKY1EDK2", HW_Q_REGS, "/_SB_/SUB5/CUB5", "0x91e8000" },
    { "CIXTEK SKY1EDK2", HW_Q_NOIRQ, "/_SB_/SUB5/CUB5", "/_SB_/PDC0" },
    { "CIXTEK SKY1EDK2", HW_Q_ALIAS, "/_SB_/XHC5", "/_SB_/SUB5/CUB5" },
    { "CIXTEK SKY1EDK2", HW_Q_FIRMWARE, "/_SB_/HUB0", "UEFI's: host mode" },
    { "CIXTEK SKY1EDK2", HW_Q_REGS, "/_SB_/HUB0/CUB0", "0x9268000" },
    { "CIXTEK SKY1EDK2", HW_Q_NOIRQ, "/_SB_/HUB0/CUB0", "/_SB_/PDC0" },
    { "CIXTEK SKY1EDK2", HW_Q_ALIAS, "/_SB_/USB0", "/_SB_/HUB0/CUB0" },
    { "CIXTEK SKY1EDK2", HW_Q_FIRMWARE, "/_SB_/HUB1", "UEFI's: host mode" },
    { "CIXTEK SKY1EDK2", HW_Q_REGS, "/_SB_/HUB1/CUB1", "0x9298000" },
    { "CIXTEK SKY1EDK2", HW_Q_NOIRQ, "/_SB_/HUB1/CUB1", "/_SB_/PDC0" },
    { "CIXTEK SKY1EDK2", HW_Q_ALIAS, "/_SB_/USB1", "/_SB_/HUB1/CUB1" },
    { "CIXTEK SKY1EDK2", HW_Q_FIRMWARE, "/_SB_/HUB2", "UEFI's: host mode" },
    { "CIXTEK SKY1EDK2", HW_Q_REGS, "/_SB_/HUB2/CUB2", "0x92c8000" },
    { "CIXTEK SKY1EDK2", HW_Q_NOIRQ, "/_SB_/HUB2/CUB2", "/_SB_/PDC0" },
    { "CIXTEK SKY1EDK2", HW_Q_ALIAS, "/_SB_/USB2", "/_SB_/HUB2/CUB2" },
    { "CIXTEK SKY1EDK2", HW_Q_FIRMWARE, "/_SB_/HUB3", "UEFI's: host mode" },
    { "CIXTEK SKY1EDK2", HW_Q_REGS, "/_SB_/HUB3/CUB3", "0x92f8000" },
    { "CIXTEK SKY1EDK2", HW_Q_NOIRQ, "/_SB_/HUB3/CUB3", "/_SB_/PDC0" },
    { "CIXTEK SKY1EDK2", HW_Q_ALIAS, "/_SB_/USB3", "/_SB_/HUB3/CUB3" },
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
        case HW_Q_POWER:
            g->nodes[id].power_set = q->arg;
            break;
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
        case HW_Q_IRQ: {
            const char *c = q->arg;
            uint64_t line = hex(&c);
            hw_id ctlr;
            struct hw_res *r;
            struct hw_ref *f;

            ctlr = hw_find_path(g, c, hw_strlen(c));
            if (ctlr == HW_NONE || !(r = hw_res_add(g, id)) ||
                !(f = hw_ref_add(g, id, HW_REF_IRQ, "interrupts", true))) {
                g->broken++;
                continue;
            }
            *r = (struct hw_res){ .kind = HW_IRQ, .ctlr = ctlr, .base = line, .ncells = 1,
                                  .cells = { (uint32_t)line } };
            f->target = f->supplier = ctlr;
            break;
        }
        case HW_Q_SCANOUT: {
            hw_id fb = HW_NONE;

            /* hwdb's node for the framebuffer the firmware left lit, if
             * it left one (server.c's framebuffer()). */
            for (hw_id i = 1; i < g->count && fb == HW_NONE; i++)
                if (hw_has_key(&g->nodes[i], "simple-framebuffer"))
                    fb = i;
            if (fb == HW_NONE)
                continue;
            g->nodes[id].with = fb;
            break;
        }
        case HW_Q_NOIRQ: {
            /* All its lines on the controller, references and all; or
             * one line, when the arg names it, the rest kept. */
            struct hw_node *d = &g->nodes[id];
            const char *c = q->arg;
            bool one = c[0] == '0' && (c[1] | 0x20) == 'x';
            uint64_t line = one ? hex(&c) : 0;
            hw_id ctlr = hw_find_path(g, c, hw_strlen(c));
            unsigned kept = 0;

            if (ctlr == HW_NONE) {
                g->broken++;
                continue;
            }
            for (unsigned k = 0; k < d->nres; k++)
                if (d->res[k].kind != HW_IRQ || d->res[k].ctlr != ctlr ||
                    (one && d->res[k].base != line))
                    d->res[kept++] = d->res[k];
            d->nres = (uint16_t)kept;
            for (unsigned k = 0; k < d->nrefs && !one; k++)
                if (d->refs[k].kind == HW_REF_IRQ &&
                    (d->refs[k].target == ctlr || d->refs[k].supplier == ctlr)) {
                    d->refs[k].required = false;
                    d->refs[k].dropped = true;
                }
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
