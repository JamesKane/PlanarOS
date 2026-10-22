/*
 * arch/aarch64/user/user.ld -- layout of a built-in user program
 *
 * Linked at 0 and loaded wherever the kernel puts it, as a position-independent
 * executable: lib/rt/reloc.c applies its relocations at the start. Code
 * and constants come first and end on a page boundary, so they can be
 * mapped RX and the rest RW. From omega's user.ld.
 */
OUTPUT_ARCH(aarch64)
ENTRY(_start)

SECTIONS
{
    . = 0;
    .text : {
        KEEP(*(.text.head))
        *(.text .text.*)
        *(.rodata .rodata.*)
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
    /* thread_local's first values, then its zeroed ones: the template each
     * task's block is made from (lib/rt/tls.c). Aligned to 64, which
     * lib/rt/tls.c and crt0 assume, and mk/user.mk checks. */
    .tdata : ALIGN(64) {
        __tdata_start = .;
        *(.tdata .tdata.*)
        __tdata_end = .;
    }
    .tbss : {
        *(.tbss .tbss.*)
        __tbss_end = .;
    }
    .data : {
        *(.data .data.*)
        *(.got .got.*)
    }
    .dynamic : {
        *(.dynamic)
    }
    .bss : {
        *(.bss .bss.* COMMON)
    }
    . = ALIGN(8);
    _end = .;

    /DISCARD/ : {
        *(.comment) *(.note .note.*) *(.eh_frame*)
    }
}
