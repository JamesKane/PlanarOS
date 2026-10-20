/*
 * arch/rv64/user/user.ld -- layout of a built-in user program
 *
 * Linked at 0 and loaded wherever the kernel puts it, so the code must
 * reach its data PC-relatively only (no absolute pointers in initialized
 * data). Code and constants come first and end on a page boundary, so
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
    . = ALIGN(4096);
    _etext = .;
    .data : {
        *(.data .data.* .sdata .sdata.*)
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
