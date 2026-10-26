/*
 * drv/gpu-panthor/mmu.c -- the G720's MMU: page tables and address
 * spaces (Linux's panthor_mmu.c, and io-pgtable's ARM LPAE stage 1)
 *
 * Each address space walks four levels of 4 KiB tables, AArch64's own
 * stage-1 format. The tables come from one pool, a DMA Area the CPU
 * doesn't cache, so a table written is in memory as it's written, and
 * the GPU walks them uncached (TRANSCFG's PTW_MEMATTR_NC): after a
 * change only the GPU's TLBs and caches need flushing (vm_flush).
 *
 * Every attribute is the same: inner and outer non-cacheable to the
 * GPU. The CIX patches' rule (memory for a GPU without an IOMMU that
 * isn't fully ACE coherent): the G720's L2 needn't write back what a
 * non-snooping reader, the DPU or the CPU's uncached Areas, needs.
 *
 * Address space 0 is the MCU's, laid out as the firmware asks; each
 * client's has its Areas at their own addresses (identity).
 */
#include <stddef.h>

#include <planar/lease.h>
#include <planar/syscalls.h>

#include "panthor.h"

constexpr unsigned POOL = 1024;                 /* tables: 4 MiB */
constexpr uint64_t PTE_VALID = 3;               /* a table, or a level-3 page */
constexpr uint64_t PTE_AF = 1ull << 10, PTE_NG = 1ull << 11;
constexpr uint64_t PTE_UNPRIV = 1ull << 6, PTE_RDONLY = 2ull << 6, PTE_SH_OS = 2ull << 8;
constexpr uint64_t PTE_XN = 3ull << 53;
constexpr uint64_t PTE_ADDR = 0x0000fffffffff000ull;
/* Every attribute index: inner and outer NC, the GPU's own shareability,
 * no allocation (AS_MEMATTR_AARCH64_INNER_OUTER_NC | SH_MIDGARD_INNER |
 * INNER_ALLOC_EXPL(0, 0)). */
constexpr uint64_t MEMATTR = 0x4c4c4c4c4c4c4c4cull;

static uint64_t *pool;
static uint8_t used[POOL];

static uint64_t *table_new(void)
{
    for (unsigned i = 0; i < POOL; i++)
        if (!used[i]) {
            used[i] = 1;
            memset(pool + i * 512, 0, PAGE);
            return pool + i * 512;
        }
    return nullptr;
}

static void table_free(uint64_t *t, unsigned level)
{
    if (level < 3)
        for (unsigned i = 0; i < 512; i++)
            if (t[i] & 1)
                table_free((uint64_t *)(t[i] & PTE_ADDR), level + 1);
    used[(t - pool) / 512] = 0;
}

bool mmu_init(struct drv *d)
{
    void *at;

    if (AllocDmaArea(drv_lease(d), POOL * PAGE, AREAF_NC, &at) < 0)
        return false;
    pool = at;
    return true;
}

bool vm_new(struct vm *v)
{
    v->as = -1;
    return (v->root = table_new()) != nullptr;
}

void vm_free(struct vm *v)
{
    if (v->as >= 0)
        vm_disable(v);
    if (v->root)
        table_free(v->root, 0);
    v->root = nullptr;
}

/* The level-3 entry for 'va', its tables made if 'make'. */
static uint64_t *pte(struct vm *v, uint64_t va, bool make)
{
    uint64_t *t = v->root;

    for (unsigned level = 0; level < 3; level++) {
        uint64_t *e = &t[va >> (39 - 9 * level) & 511];

        if (!(*e & 1)) {
            uint64_t *n;

            if (!make || !(n = table_new()))
                return nullptr;
            *e = (uint64_t)n | PTE_VALID;
        }
        t = (uint64_t *)(*e & PTE_ADDR);
    }
    return &t[va >> 12 & 511];
}

bool vm_map(struct vm *v, uint64_t va, uint64_t pa, uint64_t size, bool write, bool exec)
{
    uint64_t bits = PTE_VALID | PTE_AF | PTE_NG | PTE_UNPRIV | PTE_SH_OS |
                    (write ? 0 : PTE_RDONLY) | (exec ? 0 : PTE_XN);

    if ((va | pa | size) & (PAGE - 1) || va + size < va || (va + size) >> gpu.va_bits)
        return false;
    for (uint64_t off = 0; off < size; off += PAGE) {
        uint64_t *e = pte(v, va + off, true);

        if (!e) {
            vm_unmap(v, va, off);
            return false;
        }
        *e = (pa + off) | bits;
    }
    drv_io_barrier();
    vm_flush(v);
    return true;
}

void vm_unmap(struct vm *v, uint64_t va, uint64_t size)
{
    for (uint64_t off = 0; off < size; off += PAGE) {
        uint64_t *e = pte(v, va + off, false);

        if (e)
            *e = 0;
    }
    drv_io_barrier();
    vm_flush(v);
}

/* --- Address spaces. --- */

static uint32_t asr(int as, uint32_t reg)
{
    return gr(MMU_AS0 + 0x40 * (uint32_t)as + reg);
}

static void asw(int as, uint32_t reg, uint32_t v)
{
    gw(MMU_AS0 + 0x40 * (uint32_t)as + reg, v);
}

static bool as_ready(int as)
{
    for (uint64_t until = CurrentTime() + 100 * MS; asr(as, AS_STATUS) & AS_ACTIVE;)
        if (CurrentTime() > until) {
            say("address space ", (uint64_t)as, 10, "'s command is stuck");
            return false;
        }
    return true;
}

static bool as_cmd(int as, uint32_t cmd)
{
    if (!as_ready(as))
        return false;
    asw(as, AS_COMMAND, cmd);
    return true;
}

/* The caches cleaned and invalidated, with the address space locked
 * round it, so its TLBs are flushed too (panthor's
 * mmu_hw_do_operation_locked, over every address). */
static bool as_flush(int as)
{
    bool ok;

    gw64(MMU_AS0 + 0x40 * (uint32_t)as + AS_LOCKADDR, 63);     /* all of it */
    if (!as_cmd(as, AS_CMD_LOCK) || !as_ready(as))
        return false;
    gw(GPU_INT_CLEAR, GPU_IRQ_CLEAN_CACHES_COMPLETED);
    gw(GPU_CMD, GPU_FLUSH_ALL);
    ok = true;
    for (uint64_t until = CurrentTime() + 100 * MS;
         !(gr(GPU_INT_RAWSTAT) & GPU_IRQ_CLEAN_CACHES_COMPLETED);)
        if (CurrentTime() > until) {
            glog("the caches wouldn't flush");
            ok = false;
            break;
        }
    gw(GPU_INT_CLEAR, GPU_IRQ_CLEAN_CACHES_COMPLETED);
    return as_cmd(as, AS_CMD_UNLOCK) && as_ready(as) && ok;
}

void vm_flush(struct vm *v)
{
    if (v->as >= 0)
        as_flush(v->as);
}

bool vm_enable(struct vm *v, int as)
{
    uint64_t cfg = TRANSCFG_4K | (uint64_t)(55 - gpu.va_bits) << 6 | TRANSCFG_PTW_NC;

    if (!as_flush(as))
        return false;
    gw64(MMU_AS0 + 0x40 * (uint32_t)as + AS_TRANSTAB, (uint64_t)v->root);
    gw64(MMU_AS0 + 0x40 * (uint32_t)as + AS_MEMATTR, MEMATTR);
    gw64(MMU_AS0 + 0x40 * (uint32_t)as + AS_TRANSCFG, cfg);
    if (!as_cmd(as, AS_CMD_UPDATE) || !as_ready(as))
        return false;
    v->as = as;
    gw(MMU_INT_CLEAR, 1u << as);
    gw(MMU_INT_MASK, gr(MMU_INT_MASK) | 1u << as);
    return true;
}

void vm_disable(struct vm *v)
{
    int as = v->as;

    if (as < 0)
        return;
    gw(MMU_INT_MASK, gr(MMU_INT_MASK) & ~(1u << as));
    as_flush(as);
    gw64(MMU_AS0 + 0x40 * (uint32_t)as + AS_TRANSTAB, 0);
    gw64(MMU_AS0 + 0x40 * (uint32_t)as + AS_MEMATTR, 0);
    gw64(MMU_AS0 + 0x40 * (uint32_t)as + AS_TRANSCFG, TRANSCFG_UNMAPPED);
    as_cmd(as, AS_CMD_UPDATE);
    as_ready(as);
    v->as = -1;
}

/* A fault: the client whose address space it was loses its queue; the
 * MCU's is the firmware's own, and the GPU is given up. */
void mmu_interrupt(void)
{
    uint32_t st = gr(MMU_INT_STAT) & 0xffff;

    while (st) {
        int as = __builtin_ctz(st);
        uint32_t status = asr(as, AS_FAULTSTATUS);
        uint64_t addr = asr(as, AS_FAULTADDRESS) | (uint64_t)asr(as, AS_FAULTADDRESS + 4) << 32;

        st &= st - 1;
        say("a fault in address space ", (uint64_t)as, 10, "");
        say("  status ", status, 16, "");
        say("  at ", addr, 16, "");
        if (as == 0) {
            gpu.broken = true;
            gw(MMU_INT_MASK, gr(MMU_INT_MASK) & ~1u);
        }
        for (unsigned i = 0; i < GPU_CLIENTS; i++) {
            struct client *c = &gpu.clients[i];

            if (c->open && c->vm.as == as) {
                c->fault = status ? status : 1;
                c->fault_addr = addr;
                c->faults++;
                vm_disable(&c->vm);
            }
        }
        gw(MMU_INT_CLEAR, 1u << as);
    }
}
