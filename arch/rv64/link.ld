/*
 * arch/rv64/link.ld -- kernel image layout
 *
 * LOAD_ADDR comes from mk/<platform>.mk through --defsym. The kernel is
 * loaded there (physical) and runs at the same address in the direct map
 * (KERNEL_VOFFSET + LOAD_ADDR); see entry.S.
 */
KERNEL_VOFFSET = 0xffffffc000000000;
OUTPUT_ARCH(riscv)
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
        *(.rodata .rodata.* .srodata .srodata.*)
        . = ALIGN(8);
        __fixup_start = .;
        KEEP(*(.fixup_table))
        __fixup_end = .;
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
