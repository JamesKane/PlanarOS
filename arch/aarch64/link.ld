/*
 * arch/aarch64/link.ld -- kernel image layout
 *
 * LOAD_ADDR comes from mk/<platform>.mk through --defsym: the start of
 * RAM plus the header's text_offset. V0 runs at its physical load address
 * with the MMU off (entry.S).
 */
OUTPUT_ARCH(aarch64)
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
        *(.rodata .rodata.*)
    }

    .data : ALIGN(16) {
        *(.data .data.*)
    }

    .bss (NOLOAD) : ALIGN(16) {
        __bss_start = .;
        *(.bss .bss.* COMMON)
        . = ALIGN(16);
        __bss_end = .;
    }

    _kernel_end = .;
    _image_size = __bss_end - _kernel_start;

    /DISCARD/ : {
        *(.comment) *(.note .note.*) *(.eh_frame*)
    }
}
