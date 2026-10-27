/*
 * arch/rv64/user/user.ld -- layout of a built-in user program
 *
 * Linked at 0 and loaded wherever the kernel puts it, as a position-independent
 * executable: lib/rt/reloc.c applies its relocations at the start. Code
 * and constants come first and end on a page boundary, so they can be
 * mapped RX and the rest RW. From omega's user.ld.
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
    /* planar-c's unwind table and the build-id note, for crash records
     * (docs §19.4), in the code; crt0's header points at both. Empty
     * from clang: its functions are found by the frame-pointer chain. */
    .planar.unwind : ALIGN(4) {
        __planar_unwind = .;
        KEEP(*(.planar.unwind))
        __planar_unwind_end = .;
    }
    .note.gnu.build-id : ALIGN(4) {
        __build_id = .;
        KEEP(*(.note.gnu.build-id))
        __build_id_end = .;
    }
    . = ALIGN(4096);
    _etext = .;
    /* thread_local's first values, then its zeroed ones: the template each
     * task's block is made from (lib/rt/tls.c). Aligned to 64, which
     * lib/rt/tls.c and crt0 assume, and mk/user.mk checks. .tbss is too,
     * since .tdata is dropped when it's empty, and its alignment with it. */
    .tdata : ALIGN(64) {
        __tdata_start = .;
        *(.tdata .tdata.*)
        __tdata_end = .;
    }
    .tbss : ALIGN(64) {
        *(.tbss .tbss.*)
        __tbss_end = .;
    }
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
