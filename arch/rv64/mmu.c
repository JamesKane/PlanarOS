/*
 * arch/rv64/mmu.c -- Sv39 protection domains
 *
 * Each domain's root table starts as a copy of the kernel's (entry.S's
 * boot_root): the upper half points at the kernel's gigapages, which never
 * change, so the copy never goes stale. The lower half is the domain's
 * own. ASIDs are 16 bits on the X60 (target.md V1); a domain keeps its
 * ASID for life and switching is one satp write. On a hart without
 * ASIDs every domain uses ASID 0 and a switch flushes the TLB.
 *
 * sfence.vma is local to a hart: unmapping a page or destroying a domain
 * fences every hart through SBI RFENCE. A new mapping is fenced only
 * here; another hart that cached the old, invalid entry takes a fault,
 * which riscv_fault_spurious() recognises and retries. From omega's
 * arch/riscv64/mmu.c.
 */
#include <stdatomic.h>

#include "arch.h"
#include "cpu.h"
#include "pmm.h"
#include "riscv.h"
#include "sbi.h"

constexpr unsigned PTES   = 512;
constexpr uint64_t PTE_V  = 1ULL << 0;
constexpr uint64_t PTE_R  = 1ULL << 1;
constexpr uint64_t PTE_W  = 1ULL << 2;
constexpr uint64_t PTE_X  = 1ULL << 3;
constexpr uint64_t PTE_U  = 1ULL << 4;
constexpr uint64_t PTE_A  = 1ULL << 6;
constexpr uint64_t PTE_D  = 1ULL << 7;
constexpr uint64_t PTE_LEAF = PTE_R | PTE_W | PTE_X;
constexpr uint64_t PTE_PBMT_NC = 1ULL << 61;   /* Svpbmt: non-cacheable, idempotent memory */
constexpr uint64_t SATP_SV39 = 8ULL << 60;
constexpr unsigned SATP_ASID_SHIFT = 44;

typedef uint64_t pte_t;

extern pte_t boot_root[PTES];       /* entry.S */

static spinlock_t asid_lock;
static uint64_t asid_used[65536 / 64] = { 1 };  /* ASID 0 is the kernel's */
static unsigned asid_count = 1;     /* 1 << the hart's ASID bits */
static unsigned asids_live;

static pte_t *table(uint64_t pa)
{
    return phys_to_virt(pa);
}

static uint64_t pte_pa(pte_t pte)
{
    return (pte >> 10) << PAGE_SHIFT;
}

/* How many ASID bits the hart has: write all ones and read back. */
void riscv_probe_asids(void)
{
    uint64_t satp = csr_read_satp(), back;

    csr_write_satp(satp | 0xffffULL << SATP_ASID_SHIFT);
    back = csr_read_satp();
    csr_write_satp(satp);
    sfence_vma_all();
    asid_count = (unsigned)((back >> SATP_ASID_SHIFT) & 0xffff) + 1;
}

unsigned arch_asid_bits(void)
{
    return asid_count > 1 ? riscv_asid_bits() : 0;
}

unsigned riscv_asid_bits(void)
{
    return (unsigned)__builtin_ctz(asid_count);
}

static uint32_t asid_alloc(void)
{
    unsigned long was = spin_lock_irqsave(&asid_lock);

    asids_live++;
    if (asid_count > 1) {
        for (unsigned w = 0; w < asid_count / 64 + 1; w++) {
            if (asid_used[w] != ~0ULL) {
                unsigned b = (unsigned)__builtin_ctzll(~asid_used[w]);
                uint32_t asid = w * 64 + b;

                if (asid >= asid_count)
                    break;
                asid_used[w] |= 1ULL << b;
                spin_unlock_irqrestore(&asid_lock, was);
                return asid;
            }
        }
        asids_live--;
        spin_unlock_irqrestore(&asid_lock, was);
        return ~0u;
    }
    spin_unlock_irqrestore(&asid_lock, was);
    return 0;               /* no ASIDs: everyone shares 0 and switches flush */
}

static void asid_free(uint32_t asid)
{
    unsigned long was = spin_lock_irqsave(&asid_lock);

    if (asid)
        asid_used[asid / 64] &= ~(1ULL << (asid % 64));
    asids_live--;
    spin_unlock_irqrestore(&asid_lock, was);
}

unsigned arch_domains_live(void)
{
    return asids_live;
}

/* Every hart drops the domain's translations (all of them, or one page). */
static void fence_all(uint32_t asid, uint64_t va, uint64_t size)
{
    __asm__ volatile("fence rw, rw" : : : "memory");
    sbi_call5(SBI_EXT_RFENCE, SBI_RFENCE_SFENCE_VMA_ASID, 0, SBI_ALL_HARTS, va, size, asid);
}

static unsigned count_tables(uint64_t pa, int level, unsigned limit)
{
    pte_t *t = table(pa);
    unsigned n = 1;

    for (unsigned i = 0; i < limit; i++)
        if ((t[i] & PTE_V) && !(t[i] & PTE_LEAF) && level > 0)
            n += count_tables(pte_pa(t[i]), level - 1, PTES);
    return n;
}

unsigned arch_domain_tables(struct arch_domain *d)
{
    return count_tables(d->root, 2, PTES / 2);
}

bool arch_domain_create(struct arch_domain *d)
{
    pte_t *root;

    if ((d->asid = asid_alloc()) == ~0u)
        return false;
    if (!(d->root = pmm_alloc())) {
        asid_free(d->asid);
        return false;
    }
    root = table(d->root);
    for (unsigned i = PTES / 2; i < PTES; i++)
        root[i] = boot_root[i];
    return true;
}

/* Free the lower-half tables under 'pa' at 'level' (2 = root). Leaf pages
 * belong to whoever mapped them and aren't touched. */
static void free_tables(uint64_t pa, int level, unsigned limit)
{
    pte_t *t = table(pa);

    for (unsigned i = 0; i < limit; i++)
        if ((t[i] & PTE_V) && !(t[i] & PTE_LEAF) && level > 0)
            free_tables(pte_pa(t[i]), level - 1, PTES);
    if (level < 2)
        pmm_free(pa);
}

/* No hart may have 'd' loaded (process.c guarantees it). */
void arch_domain_destroy(struct arch_domain *d)
{
    free_tables(d->root, 2, PTES / 2);
    pmm_free(d->root);
    fence_all(d->asid, 0, ~0ULL);
    asid_free(d->asid);
    *d = (struct arch_domain){ 0 };
}

/* The level-0 PTE for 'va', creating tables on the way if 'create'. */
static pte_t *walk(struct arch_domain *d, uint64_t va, bool create)
{
    pte_t *t = table(d->root);

    for (int level = 2; level > 0; level--) {
        pte_t *pte = &t[(va >> (PAGE_SHIFT + 9 * level)) & (PTES - 1)];

        if (!(*pte & PTE_V)) {
            uint64_t pa;

            if (!create || !(pa = pmm_alloc()))
                return nullptr;
            *pte = (pa >> PAGE_SHIFT) << 10 | PTE_V;
        }
        if (*pte & PTE_LEAF)
            return nullptr;         /* a large page; not made here */
        t = table(pte_pa(*pte));
    }
    return &t[(va >> PAGE_SHIFT) & (PTES - 1)];
}

bool arch_map(struct arch_domain *d, uint64_t va, uint64_t pa, uint32_t flags)
{
    pte_t *pte, bits = PTE_V | PTE_A | PTE_D;

    if (va >= ARCH_USER_TOP || (va | pa) & (PAGE_SIZE - 1))
        return false;
    if (!(pte = walk(d, va, true)))
        return false;
    bits |= flags & MAP_READ ? PTE_R : 0;
    bits |= flags & MAP_WRITE ? PTE_W : 0;
    bits |= flags & MAP_EXEC ? PTE_X : 0;
    bits |= flags & MAP_USER ? PTE_U : 0;
    /* MAP_DEVICE needs nothing: the platform's PMAs make device ranges
     * uncached I/O, on QEMU and the K1 alike. MAP_NC is Svpbmt's, which
     * area.c asks for only once arch_has_nc() has said it works. */
    bits |= flags & MAP_NC ? PTE_PBMT_NC : 0;
    if (*pte & PTE_V) {
        /* Changing a live mapping: every hart may cache the old one, so
         * take it away everywhere before the new one goes in (review H2). */
        *pte = 0;
        fence_all(d->asid, va, PAGE_SIZE);
    }
    *pte = (pa >> PAGE_SHIFT) << 10 | bits;
    __asm__ volatile("sfence.vma %0, %1" : : "r"(va), "r"((uint64_t)d->asid) : "memory");
    return true;
}

uint64_t arch_unmap(struct arch_domain *d, uint64_t va)
{
    pte_t *pte = walk(d, va, false);
    uint64_t pa;

    if (!pte || !(*pte & PTE_V))
        return 0;
    pa = pte_pa(*pte);
    *pte = 0;
    fence_all(d->asid, va, PAGE_SIZE);
    return pa;
}

uint64_t arch_lookup(struct arch_domain *d, uint64_t va)
{
    pte_t *pte = walk(d, va, false);

    return pte && (*pte & PTE_V) ? pte_pa(*pte) : 0;
}

/*
 * A user page fault this hart's TLB may have caused by caching an entry
 * that was invalid before another hart mapped the page: if the domain
 * now allows the access, fence locally and say so (the access is retried).
 */
bool arch_allows(struct arch_domain *d, uint64_t va, uint32_t flags)
{
    pte_t *pte, need = PTE_V | PTE_U;

    if (va >= ARCH_USER_TOP)
        return false;
    need |= flags & MAP_READ ? PTE_R : 0;
    need |= flags & MAP_WRITE ? PTE_W : 0;
    need |= flags & MAP_EXEC ? PTE_X : 0;
    if (!(pte = walk(d, va, false)) || (*pte & need) != need)
        return false;
    __asm__ volatile("sfence.vma %0, %1" : : "r"(va), "r"((uint64_t)d->asid) : "memory");
    return true;
}

bool riscv_fault_spurious(uint64_t va, uint64_t scause)
{
    const struct arch_domain *d = this_cpu()->loaded_domain;
    pte_t *pte, need;

    if (!d || va >= ARCH_USER_TOP)
        return false;
    need = scause == 12 ? PTE_X : scause == 15 ? PTE_W : PTE_R;
    pte = walk((struct arch_domain *)d, va, false);
    if (!pte || (*pte & (PTE_V | PTE_U | need)) != (PTE_V | PTE_U | need))
        return false;
    __asm__ volatile("sfence.vma %0, %1" : : "r"(va), "r"((uint64_t)d->asid) : "memory");
    return true;
}

void arch_domain_switch(struct arch_domain *d)
{
    struct cpu *c = this_cpu();

    if (c->loaded_domain == d)
        return;
    c->loaded_domain = d;
    if (d)
        csr_write_satp(SATP_SV39 | (uint64_t)d->asid << SATP_ASID_SHIFT | d->root >> PAGE_SHIFT);
    else
        csr_write_satp(SATP_SV39 | virt_to_phys(boot_root) >> PAGE_SHIFT);
    if (asid_count == 1)
        sfence_vma_all();
}

bool riscv_zicbom;

/*
 * Svpbmt, tried rather than read from the device tree: the K1 has it but
 * its U-Boot's tree says only "rv64imafdcv". A hart without it, or whose
 * M-mode left menvcfg.PBMTE clear, takes a page fault on a leaf with
 * PBMT bits set; a kernel-mode fault goes to the copy's fixup, never to
 * the spurious-fault retry. So: map one page so in a scratch domain and
 * read it once.
 */
bool arch_has_nc(void)
{
    static atomic_int known;        /* 1 yes, -1 no, 0 not tried */
    struct arch_domain d;
    uint64_t page, v;
    bool ok;
    int k = atomic_load(&known);

    if (k)
        return k > 0;
    if (!riscv_zicbom || !(page = pmm_alloc()))
        return false;               /* without Zicbom, the direct map's alias can't be cleaned */
    if (!arch_domain_create(&d)) {
        pmm_free(page);
        return false;
    }
    if ((ok = page < ARCH_USER_TOP && arch_map(&d, page, page, MAP_USER | MAP_READ | MAP_NC))) {
        unsigned long was = arch_irq_save();
        const struct arch_domain *old = this_cpu()->loaded_domain;

        arch_domain_switch(&d);
        ok = arch_copy_from_user(&v, page, sizeof v);
        arch_domain_switch((struct arch_domain *)old);
        arch_irq_restore(was);
    }
    arch_domain_destroy(&d);
    pmm_free(page);
    atomic_store(&known, ok ? 1 : -1);
    return ok;
}

/* cbo.flush: write back and invalidate, 64-byte blocks (the K1's and QEMU's). */
void arch_dcache_flush(const void *va, size_t len)
{
    uintptr_t a = (uintptr_t)va & ~63ULL, end = (uintptr_t)va + len;

    if (!riscv_zicbom)
        return;
    __asm__ volatile("fence rw, rw" : : : "memory");
    for (; a < end; a += 64)
        __asm__ volatile(".insn i 0x0f, 2, x0, 2(%0)" : : "r"(a) : "memory");
    __asm__ volatile("fence rw, rw" : : : "memory");
}

void arch_sync_icache(const void *, size_t)
{
    __asm__ volatile("fence.i" : : : "memory");
    sbi_call(SBI_EXT_RFENCE, SBI_RFENCE_FENCE_I, 0, SBI_ALL_HARTS, 0);
}
