/*
 * arch/rv64/user/user.ld -- layout of a built-in user program
 *
 * Linked at 0 and loaded wherever the kernel puts it, as a position-independent
 * executable: lib/rt/reloc.c applies its relocations at the start. Code and constants come first and end on a page boundary, so
 * they can be mapped RX and the rest RW. From omega's user.ld.
 */
OUTPUT_ARCH(riscv)
ENTRY(_start)

SECTIONS
{
    . = 0;
    .text : {
        KEEP(*(.text.head))
        *(.text .text.*)
        *(.rodata .rodata.* .srodata .srodata.*)
    }
    .rela.dyn : {
        __rela_start = .;
        *(.rela.dyn .rela.*)
        __rela_end = .;
    }
    .dynsym : { *(.dynsym) }
    .dynstr : { *(.dynstr) }
    .hash : { *(.hash) }
    .gnu.hash : { *(.gnu.hash) }
    . = ALIGN(4096);
    _etext = .;
    .data : {
        *(.data .data.* .sdata .sdata.*)
        *(.got .got.*)
    }
    .dynamic : {
        *(.dynamic)
    }
    .bss : {
        *(.bss .bss.* .sbss .sbss.* COMMON)
    }
    . = ALIGN(8);
    _end = .;

    /DISCARD/ : {
        *(.comment) *(.note .note.*) *(.eh_frame*) *(.riscv.attributes)
    }
}
