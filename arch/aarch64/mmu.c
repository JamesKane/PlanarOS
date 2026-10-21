/*
 * arch/aarch64/mmu.c -- VMSAv8-64 protection domains
 *
 * TTBR1 holds the kernel half (entry.S) and never changes. A domain is a
 * TTBR0 table: four levels, 4 KiB pages, 48-bit addresses, tagged with an
 * ASID (16 bits where ID_AA64MMFR0 says so, else 8), so switching is one
 * TTBR0 write. The kernel's own domain is an empty table with ASID 0.
 * User pages are non-global, never executable by the kernel (PXN), and
 * normal write-back memory, normal non-cacheable memory (MAP_NC), or
 * Device-nGnRE and never executable for a lease's registers (MAP_DEVICE). TLB invalidation uses the inner-shareable
 * TLBI forms, which reach every CPU.
 */
#include "aarch64.h"
#include "arch.h"
#include "cpu.h"
#include "pmm.h"

constexpr unsigned PTES       = 512;
constexpr uint64_t DESC_VALID = 1ULL << 0;
constexpr uint64_t DESC_TABLE = 3;          /* levels 0-2 */
constexpr uint64_t DESC_PAGE  = 3;          /* level 3 */
constexpr uint64_t ATTR_DEVICE = 1ULL << 2; /* MAIR index 1: Device-nGnRE */
constexpr uint64_t ATTR_NC   = 2ULL << 2;  /* MAIR index 2: Normal-NC, which may gather */
constexpr uint64_t ATTR_NORMAL = 3ULL << 2; /* MAIR index 3 */
constexpr uint64_t SH_INNER   = 3ULL << 8;
constexpr uint64_t AF         = 1ULL << 10;
constexpr uint64_t NG         = 1ULL << 11;
constexpr uint64_t AP_EL0     = 1ULL << 6;  /* EL0 may access */
constexpr uint64_t AP_RO      = 1ULL << 7;
constexpr uint64_t PXN        = 1ULL << 53;
constexpr uint64_t UXN        = 1ULL << 54;
constexpr uint64_t ADDR_MASK  = 0x0000fffffffff000ULL;

typedef uint64_t desc_t;

alignas(4096) static desc_t empty_root[PTES];

static spinlock_t asid_lock;
static uint64_t asid_used[65536 / 64] = { 1 };  /* ASID 0 is the kernel's */
static unsigned asids_live;

static desc_t *table(uint64_t pa)
{
    return phys_to_virt(pa);
}

static unsigned asid_count(void);

unsigned arch_asid_bits(void)
{
    return (unsigned)__builtin_ctz(asid_count());
}

static unsigned asid_count(void)
{
    return ((sysreg_read(id_aa64mmfr0_el1) >> 4) & 0xf) == 2 ? 65536 : 256;
}

static uint32_t asid_alloc(void)
{
    unsigned long was = spin_lock_irqsave(&asid_lock);
    unsigned n = asid_count();

    for (unsigned w = 0; w < n / 64; w++) {
        if (asid_used[w] != ~0ULL) {
            unsigned b = (unsigned)__builtin_ctzll(~asid_used[w]);

            asid_used[w] |= 1ULL << b;
            asids_live++;
            spin_unlock_irqrestore(&asid_lock, was);
            return w * 64 + b;
        }
    }
    spin_unlock_irqrestore(&asid_lock, was);
    return 0;
}

static void asid_free(uint32_t asid)
{
    unsigned long was = spin_lock_irqsave(&asid_lock);

    asid_used[asid / 64] &= ~(1ULL << (asid % 64));
    asids_live--;
    spin_unlock_irqrestore(&asid_lock, was);
}

unsigned arch_domains_live(void)
{
    return asids_live;
}

static unsigned count_tables(uint64_t pa, int level)
{
    desc_t *t = table(pa);
    unsigned n = 1;

    if (level < 3)
        for (unsigned i = 0; i < PTES; i++)
            if ((t[i] & 3) == DESC_TABLE)
                n += count_tables(t[i] & ADDR_MASK, level + 1);
    return n;
}

unsigned arch_domain_tables(struct arch_domain *d)
{
    return count_tables(d->root, 0);
}

bool arch_domain_create(struct arch_domain *d)
{
    if (!(d->asid = asid_alloc()))
        return false;
    if (!(d->root = pmm_alloc())) {
        asid_free(d->asid);
        return false;
    }
    return true;
}

/* Free the tables under 'pa' at 'level' (0 = root); not the pages. */
static void free_tables(uint64_t pa, int level)
{
    desc_t *t = table(pa);

    if (level < 3)
        for (unsigned i = 0; i < PTES; i++)
            if ((t[i] & 3) == DESC_TABLE)
                free_tables(t[i] & ADDR_MASK, level + 1);
    pmm_free(pa);
}

/* No CPU may have 'd' loaded (process.c guarantees it). */
void arch_domain_destroy(struct arch_domain *d)
{
    free_tables(d->root, 0);
    __asm__ volatile("dsb ishst\n\ttlbi aside1is, %0\n\tdsb ish\n\tisb"
                     : : "r"((uint64_t)d->asid << 48) : "memory");
    asid_free(d->asid);
    *d = (struct arch_domain){ 0 };
}

/* The level-3 descriptor for 'va', creating tables on the way if 'create'. */
static desc_t *walk(struct arch_domain *d, uint64_t va, bool create)
{
    desc_t *t = table(d->root);

    for (int level = 0; level < 3; level++) {
        desc_t *e = &t[(va >> (39 - 9 * level)) & (PTES - 1)];

        if (!(*e & DESC_VALID)) {
            uint64_t pa;

            if (!create || !(pa = pmm_alloc()))
                return nullptr;
            *e = pa | DESC_TABLE;
        }
        if ((*e & 3) != DESC_TABLE)
            return nullptr;         /* a block; not made here */
        t = table(*e & ADDR_MASK);
    }
    return &t[(va >> PAGE_SHIFT) & (PTES - 1)];
}

bool arch_map(struct arch_domain *d, uint64_t va, uint64_t pa, uint32_t flags)
{
    desc_t *e, bits = DESC_PAGE | AF | NG | PXN;

    if (va >= ARCH_USER_TOP || (va | pa) & (PAGE_SIZE - 1) || !(flags & MAP_READ))
        return false;
    if (!(e = walk(d, va, true)))
        return false;
    bits |= flags & MAP_DEVICE ? ATTR_DEVICE | UXN
            : flags & MAP_NC ? ATTR_NC | SH_INNER : ATTR_NORMAL | SH_INNER;
    bits |= flags & MAP_USER ? AP_EL0 : 0;
    bits |= flags & MAP_WRITE ? 0 : AP_RO;
    bits |= flags & MAP_EXEC && flags & MAP_USER ? 0 : UXN;
    if (*e & DESC_VALID) {          /* break before make */
        *e = 0;
        __asm__ volatile("dsb ishst\n\ttlbi vae1is, %0\n\tdsb ish"
                         : : "r"(va >> 12 | (uint64_t)d->asid << 48) : "memory");
    }
    *e = pa | bits;
    __asm__ volatile("dsb ishst\n\tisb" : : : "memory");
    return true;
}

uint64_t arch_unmap(struct arch_domain *d, uint64_t va)
{
    desc_t *e = walk(d, va, false);
    uint64_t pa;

    if (!e || !(*e & DESC_VALID))
        return 0;
    pa = *e & ADDR_MASK;
    *e = 0;
    __asm__ volatile("dsb ishst\n\ttlbi vae1is, %0\n\tdsb ish\n\tisb"
                     : : "r"(va >> 12 | (uint64_t)d->asid << 48) : "memory");
    return pa;
}

bool arch_allows(struct arch_domain *d, uint64_t va, uint32_t flags)
{
    desc_t *e, v;

    if (va >= ARCH_USER_TOP || !(e = walk(d, va, false)))
        return false;
    v = __atomic_load_n(e, __ATOMIC_RELAXED);
    return (v & DESC_VALID) && (v & AP_EL0) && !((flags & MAP_WRITE) && (v & AP_RO)) &&
           !((flags & MAP_EXEC) && (v & UXN));
}

uint64_t arch_lookup(struct arch_domain *d, uint64_t va)
{
    desc_t *e = walk(d, va, false);

    return e && (*e & DESC_VALID) ? *e & ADDR_MASK : 0;
}

void arch_domain_switch(struct arch_domain *d)
{
    struct cpu *c = this_cpu();

    if (c->loaded_domain == d && d)
        return;
    c->loaded_domain = d;
    if (d)
        sysreg_write(ttbr0_el1, d->root | (uint64_t)d->asid << 48);
    else
        sysreg_write(ttbr0_el1, virt_to_phys(empty_root));
    isb();
}

/* Clean the data cache to the point of unification and invalidate the
 * instruction cache over the range, on every CPU (the IS forms). */
void arch_sync_icache(const void *va, size_t len)
{
    uint64_t ctr = sysreg_read(ctr_el0);
    uintptr_t dline = 4u << ((ctr >> 16) & 0xf), iline = 4u << (ctr & 0xf);
    uintptr_t start = (uintptr_t)va, end = start + len;

    for (uintptr_t a = start & ~(dline - 1); a < end; a += dline)
        __asm__ volatile("dc cvau, %0" : : "r"(a) : "memory");
    __asm__ volatile("dsb ish" : : : "memory");
    for (uintptr_t a = start & ~(iline - 1); a < end; a += iline)
        __asm__ volatile("ic ivau, %0" : : "r"(a) : "memory");
    __asm__ volatile("dsb ish\n\tisb" : : : "memory");
}

bool arch_has_nc(void)
{
    return true;
}

/* Clean and invalidate to the point of coherency (memory), every CPU. */
void arch_dcache_flush(const void *va, size_t len)
{
    uintptr_t line = 4u << ((sysreg_read(ctr_el0) >> 16) & 0xf);
    uintptr_t start = (uintptr_t)va, end = start + len;

    __asm__ volatile("dsb sy" : : : "memory");
    for (uintptr_t a = start & ~(line - 1); a < end; a += line)
        __asm__ volatile("dc civac, %0" : : "r"(a) : "memory");
    __asm__ volatile("dsb sy" : : : "memory");
}
