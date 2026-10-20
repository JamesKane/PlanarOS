/*
 * lib/rt/reloc.c -- a program relocates itself
 *
 * User programs are linked at 0 as position-independent executables and
 * loaded wherever the kernel puts them, so tables of pointers in their
 * data (hwdb's match tables) need the load address added. The linker
 * leaves one R_*_RELATIVE entry per such pointer; crt0 calls this before
 * main, so it touches nothing the entries fix, with the ISA's number for
 * that type (R_AARCH64_RELATIVE is 1027, R_RISCV_RELATIVE 3).
 */
#include <stddef.h>
#include <stdint.h>

struct rela {
    uint64_t offset, info;
    int64_t addend;
};

/* From user.ld; hidden, so reached PC-relatively. */
extern const struct rela __rela_start[] __attribute__((visibility("hidden")));
extern const struct rela __rela_end[] __attribute__((visibility("hidden")));
extern const char _start[] __attribute__((visibility("hidden")));

void _relocate(uint32_t relative)
{
    uint64_t base = (uint64_t)_start;

    for (const struct rela *r = __rela_start; r < __rela_end; r++)
        if ((uint32_t)r->info == relative)
            *(uint64_t *)(base + r->offset) = base + (uint64_t)r->addend;
}
