/*
 * arch/aarch64/link.ld -- kernel image layout
 *
 * LOAD_ADDR comes from mk/<platform>.mk through --defsym: the start of
 * RAM plus the header's text_offset. The kernel is loaded there
 * (physical) and runs at the same address in the direct map
 * (KERNEL_VOFFSET + LOAD_ADDR); see entry.S.
 */
KERNEL_VOFFSET = 0xffff000000000000;
OUTPUT_ARCH(aarch64)
ENTRY(_start)

SECTIONS
{
    . = KERNEL_VOFFSET + LOAD_ADDR;
    _kernel_start = .;

    .text : AT(LOAD_ADDR) {
        KEEP(*(.text.head))
        *(.text .text.*)
    }

    .rodata : ALIGN(16) {
        *(.rodata .rodata.*)
        . = ALIGN(8);
        __fixup_start = .;
        KEEP(*(.fixup_table))
        __fixup_end = .;
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
