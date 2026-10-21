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
     * The K1 under its vendor U-Boot (the root says only "ky,x1"): U-Boot
     * sets up the clocks, resets and pins of what it boots from, and
     * leaves them so. V4 drives SD (step 5) before `power` takes these
     * over (step 7), so until then they count as the firmware's.
     */
    { "ky,x1", HW_Q_FIRMWARE, "/soc/clock-controller@d4050000", "U-Boot's, until power (V4 step 7)" },
    { "ky,x1", HW_Q_FIRMWARE, "/soc/reset-controller@d4050000", "U-Boot's, until power (V4 step 7)" },
    { "ky,x1", HW_Q_FIRMWARE, "/soc/pinctrl@d401e000", "U-Boot's pins" },
    /* Its GPIOs: the SD slot's card detect is one, which sdhci does
     * without (it asks the card). */
    { "ky,x1", HW_Q_FIRMWARE, "/soc/gpio@d4019000", "U-Boot's, until a GPIO driver" },

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
