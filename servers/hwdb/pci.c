/*
 * servers/hwdb/pci.c -- the pci server, as hwdb serves it (§11.6, v4-plan step 5)
 *
 * A host bridge's driver is a bus driver and an hwdb source, as acpid
 * is. While it binds it asks hwdb what the bridge decodes
 * (HWDB_PCI_HOST): its buses, from the ECAM resource, and its windows.
 * Then it sends each function it finds (HWDB_PCI_FUNCTION), and hwdb
 * adds a node for it under the bridge, with its BARs as registers, and
 * leases, matches and starts it like any other (hw_add_late) once the
 * bridge is bound.
 *
 * pci is a driver, so what it says is checked: the function must be on
 * one of the bridge's buses, and each BAR inside one of its memory
 * windows, so that pci can't have hwdb lease registers the bridge
 * doesn't decode. A function sent again (pci restarted) is the node it
 * already has.
 *
 * A function with MSI-X, behind a bridge whose MSIs reach an ITS (its
 * msi-map, or the IORT), gets the LPIs for its first PCI_MSI_LINES
 * vectors from the kernel (AllocMsi), as interrupt resources: they go
 * in its lease as lines, and its driver fills the table.
 *
 * Keys follow the PCI bus binding to Open Firmware: "pciVVVV,DDDD",
 * then "pciclass,CCSSPP" and "pciclass,CCSS", most specific first, so
 * a class driver (NVMe's 010802) takes what no vendor's driver does.
 */
#include <planar/syscalls.h>

#include "server.h"

constexpr uint32_t PCI_MSI_LINES = 4;       /* vectors a function's lease carries */

static const struct hw_res *res_of(const struct hw_node *n, enum hw_res_kind kind, unsigned k)
{
    for (unsigned i = 0; i < n->nres; i++)
        if (n->res[i].kind == kind && k-- == 0)
            return &n->res[i];
    return nullptr;
}

static hw_id bridge_of(const struct unit *u, uint32_t device)
{
    const struct hw_lease *ls = &hwdb.g.leases[u - hwdb.units];

    return device < ls->nnodes ? ls->nodes[device] : HW_NONE;
}

bool pci_host(const struct unit *u, struct HwdbMsg *m)
{
    hw_id b = bridge_of(u, m->hm_Host.ph_Device);
    const struct hw_node *n;
    const struct hw_res *ecam, *w;
    struct PciHost *h = &m->hm_Host;

    if (b == HW_NONE || !(ecam = res_of(n = &hwdb.g.nodes[b], HW_ECAM, 0)))
        return false;
    h->ph_Segment = ecam->cells[0];
    h->ph_FirstBus = ecam->cells[1];
    h->ph_LastBus = ecam->cells[2];
    h->ph_NWindows = 0;
    for (unsigned k = 0; (w = res_of(n, HW_WINDOW, k)) && h->ph_NWindows < PCI_WINDOWS; k++)
        h->ph_Window[h->ph_NWindows++] = (struct PciWindow){
            .pw_Base = w->base, .pw_Size = w->size,
            .pw_Bus = (uint64_t)w->cells[1] << 32 | w->cells[2],
            .pw_Flags = (w->cells[0] & HW_WIN_MEM ? PCI_MEM : 0) |
                        (w->cells[0] & HW_WIN_64 ? PCI_64 : 0) |
                        (w->cells[0] & HW_WIN_PREFETCH ? PCI_PREFETCH : 0),
        };
    h->ph_Msi = res_of(n, HW_MSIMAP, 0) != nullptr;
    return true;
}

/* --- Functions. --- */

static void put_hex(struct hw_line *l, uint32_t v, unsigned digits)
{
    char s[9];

    for (unsigned i = 0; i < digits; i++)
        s[i] = "0123456789abcdef"[v >> 4 * (digits - 1 - i) & 0xf];
    s[digits] = '\0';
    hw_put(l, s);
}

static bool in_window(const struct hw_node *n, uint64_t base, uint64_t size)
{
    const struct hw_res *w;

    for (unsigned k = 0; (w = res_of(n, HW_WINDOW, k)); k++)
        if (w->cells[0] & HW_WIN_MEM && base >= w->base && size <= w->size &&
            base - w->base <= w->size - size)
            return true;
    return false;
}

static bool key(hw_id id, struct hw_line *l)
{
    return hw_key_add(&hwdb.g, id, hw_strdup(&hwdb.g, l->text, l->len));
}

/* The MSI device ID the bridge's map gives requester 'rid', and its ITS. */
static bool msi_device(const struct hw_node *b, uint32_t rid, uint32_t *dev, hw_id *its)
{
    const struct hw_res *m;

    for (unsigned k = 0; (m = res_of(b, HW_MSIMAP, k)); k++)
        if (rid >= m->base && rid - m->base < m->size) {
            *dev = m->cells[0] + (uint32_t)(rid - m->base);
            *its = m->ctlr;
            return true;
        }
    return false;
}

/* 0, or why the function was refused. */
int64_t pci_function(const struct unit *u, const struct HwdbMsg *m)
{
    struct PciFunction f = m->hm_Function;      /* the driver's memory: copied once */
    struct hw_graph *g = &hwdb.g;
    hw_id b = bridge_of(u, f.pf_Device), id;
    const struct hw_res *ecam;
    struct hw_line name = { 0 }, l = { 0 };
    struct hw_ref *r;

    if (b == HW_NONE || !(ecam = res_of(&g->nodes[b], HW_ECAM, 0)) || f.pf_Rid > 0xffff ||
        f.pf_Rid >> 8 < ecam->cells[1] || f.pf_Rid >> 8 > ecam->cells[2])
        return ERR_BADARG;
    for (unsigned i = 0; i < PCI_BARS; i++) {
        const struct PciBar *bar = &f.pf_Bar[i];

        if (bar->pb_Size && (!(bar->pb_Flags & PCI_MEM) || bar->pb_Size & (bar->pb_Size - 1) ||
                             !in_window(&g->nodes[b], bar->pb_Base, bar->pb_Size)))
            return ERR_RIGHTS;
    }
    if (f.pf_MsixVectors && (f.pf_MsixBar >= PCI_BARS || f.pf_MsixVectors > 2048 ||
                             f.pf_MsixOffset + 16 * f.pf_MsixVectors > f.pf_Bar[f.pf_MsixBar].pb_Size))
        return ERR_BADARG;

    put_hex(&name, f.pf_Rid >> 8, 2);
    hw_put(&name, ":");
    put_hex(&name, f.pf_Rid >> 3 & 0x1f, 2);
    hw_put(&name, ".");
    put_hex(&name, f.pf_Rid & 7, 1);
    if (hw_child(g, b, name.text, name.len) != HW_NONE)
        return 0;                       /* pci again, after a restart */
    if ((id = hw_node_new(g, b, hw_strdup(g, name.text, name.len))) == HW_NONE)
        return ERR_NOMEM;

    hw_put(&l, "pci");
    put_hex(&l, f.pf_Vendor, 4);
    hw_put(&l, ",");
    put_hex(&l, f.pf_Id, 4);
    key(id, &l);
    l = (struct hw_line){ 0 };
    hw_put(&l, "pciclass,");
    put_hex(&l, f.pf_Class, 6);
    key(id, &l);
    l.len -= 2;
    l.text[l.len] = '\0';
    key(id, &l);

    for (unsigned i = 0; i < PCI_BARS; i++) {
        struct hw_res *x;

        if (!f.pf_Bar[i].pb_Size || !(x = hw_res_add(g, id)))
            continue;
        x->kind = HW_MMIO;
        x->base = f.pf_Bar[i].pb_Base;
        x->size = f.pf_Bar[i].pb_Size;
    }
    if (f.pf_MsixVectors) {
        struct hw_res *x = hw_res_add(g, id);
        struct MsiInfo mi;
        uint32_t dev;
        hw_id its;

        if (x) {
            x->kind = HW_MSIX;
            x->base = f.pf_Bar[f.pf_MsixBar].pb_Base + f.pf_MsixOffset;
            x->size = f.pf_MsixVectors;
        }
        /* Its first few vectors' LPIs, as lines its lease carries. */
        if (x && msi_device(&g->nodes[b], f.pf_Rid, &dev, &its) &&
            AllocMsi(hwdb.root, dev,
                     f.pf_MsixVectors < PCI_MSI_LINES ? f.pf_MsixVectors : PCI_MSI_LINES, &mi) == 0) {
            x->ncells = 2;
            x->cells[0] = (uint32_t)(mi.mi_Doorbell >> 32);
            x->cells[1] = (uint32_t)mi.mi_Doorbell;
            for (uint32_t i = 0; i < mi.mi_Count; i++) {
                struct hw_res *q = hw_res_add(g, id);

                if (!q)
                    break;
                q->kind = HW_IRQ;
                q->ctlr = its;
                q->base = mi.mi_First + i;
                q->ncells = 1;
                q->cells[0] = i;
            }
        }
    }
    if ((r = hw_ref_add(g, id, HW_REF_PARENT, "parent", true))) {
        r->target = b;
        r->supplier = b;
    }
    /* Its DMA goes through the bridge's IOMMU, which must pass it (server.c). */
    for (unsigned k = 0; k < g->nodes[b].nrefs; k++)
        if (g->nodes[b].refs[k].kind == HW_REF_IOMMU && g->nodes[b].refs[k].supplier != HW_NONE &&
            (r = hw_ref_add(g, id, HW_REF_IOMMU, "iommu", true))) {
            r->target = g->nodes[b].refs[k].target;
            r->supplier = g->nodes[b].refs[k].supplier;
        }
    if (g->full)
        return ERR_NOMEM;
    hw_add_late(g, id);
    return 0;
}
