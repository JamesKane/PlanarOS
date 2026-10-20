/*
 * arch/rv64/link.ld -- kernel image layout
 *
 * LOAD_ADDR comes from mk/<platform>.mk through --defsym. V0 runs at its
 * physical load address with the MMU off (entry.S).
 */
OUTPUT_ARCH(riscv)
ENTRY(_start)

SECTIONS
{
    . = LOAD_ADDR;
    _kernel_start = .;

    .text : {
        KEEP(*(.text.head))
        *(.text .text.*)
    }

    .rodata : ALIGN(16) {
        *(.rodata .rodata.* .srodata .srodata.*)
    }

    .data : ALIGN(16) {
        *(.data .data.* .sdata .sdata.*)
    }

    .bss (NOLOAD) : ALIGN(16) {
        __bss_start = .;
        *(.bss .bss.* .sbss .sbss.* COMMON)
        . = ALIGN(16);
        __bss_end = .;
    }

    _kernel_end = .;
    _image_size = __bss_end - _kernel_start;

    /DISCARD/ : {
        *(.comment) *(.note .note.*) *(.eh_frame*) *(.riscv.attributes)
    }
}
