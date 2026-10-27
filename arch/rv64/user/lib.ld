/*
 * arch/rv64/user/lib.ld -- layout of a shared library (planar/libimage.h)
 *
 * Linked at 0 as a position-independent image, which the kernel
 * relocates once at boot into the shared library region, read and
 * execute in every process. Code and constants only: the relocated
 * pointers' sections (.data.rel.ro, .got) go with the constants, since
 * the kernel writes them before anyone runs, and a library with any
 * other data or .bss isn't linked. What a library keeps per process is
 * the caller's (lib/exec/procdata.h).
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
        *(.data.rel.ro .data.rel.ro.*)
        *(.got .got.*)
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
    .dynamic : { *(.dynamic) }
    /* planar-c's unwind table and the build-id note, for crash records
     * (docs §19.4), in the code; lib/libhead.S's header points at both. Empty
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
    _etext = .;
    .data : {
        *(.data .data.* .sdata .sdata.* .tdata .tdata.* .tbss .tbss.*)
    }
    .bss : {
        *(.bss .bss.* .sbss .sbss.* COMMON)
    }
    _end = .;
    ASSERT(_end == _etext, "a shared library has data or .bss: it must be code and constants")

    /DISCARD/ : {
        *(.comment) *(.note .note.*) *(.eh_frame*) *(.riscv.attributes)
    }
}
