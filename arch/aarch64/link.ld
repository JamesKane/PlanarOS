/*
 * arch/aarch64/link.ld -- kernel image layout
 *
 * The image runs wherever it's loaded: at physical P, it runs at the
 * same address in the direct map (KERNEL_VOFFSET + P; see entry.S). It's
 * linked as if P were 0, and entry.S applies its relative relocations.
 *
 * The file is also a PE/COFF EFI application (entry.S, efi.c), so its
 * layout is the PE's: the headers in the first page, then a read-execute
 * section (code, constants, relocations) and a read-write one (data,
 * .bss), each starting on a page. File offsets equal addresses, so the
 * flat binary is the PE file.
 */
KERNEL_VOFFSET = 0xffff000000000000;
OUTPUT_ARCH(aarch64)
ENTRY(_start)

SECTIONS
{
    . = KERNEL_VOFFSET;
    _kernel_start = .;

    .head : {
        KEEP(*(.text.head))
    }

    . = ALIGN(4096);
    _text = .;
    .text : {
        *(.text .text.*)
    }

    .rodata : ALIGN(16) {
        *(.rodata .rodata.*)
        . = ALIGN(8);
        __fixup_start = .;
        KEEP(*(.fixup_table))
        __fixup_end = .;
    }

    .rela.dyn : ALIGN(8) {
        __rela_start = .;
        *(.rela.dyn .rela.*)
        __rela_end = .;
    }
    .dynsym : { *(.dynsym) }
    .dynstr : { *(.dynstr) }
    .hash : { *(.hash) }
    .gnu.hash : { *(.gnu.hash) }
    .dynamic : { *(.dynamic) }

    . = ALIGN(4096);
    _etext = .;
    .data : {
        *(.data .data.*)
        *(.got .got.*)
        . = ALIGN(4096);        /* in the file: the PE's raw data ends here */
    }
    _edata = .;

    .bss (NOLOAD) : ALIGN(16) {
        __bss_start = .;
        *(.bss .bss.* COMMON)
        . = ALIGN(16);
        __bss_end = .;
    }

    _kernel_end = .;
    _image_size = ABSOLUTE(__bss_end - _kernel_start);

    /* For the PE header (entry.S). */
    __pe_text_size  = ABSOLUTE(_etext - _text);
    __pe_data_size  = ABSOLUTE(_edata - _etext);
    __pe_data_vsize = ABSOLUTE(ALIGN(__bss_end, 4096) - _etext);
    __pe_bss_size   = ABSOLUTE(ALIGN(__bss_end, 4096) - _edata);
    __pe_image_size = ABSOLUTE(ALIGN(__bss_end, 4096) - _kernel_start);
    __pe_text_rva   = ABSOLUTE(_text - _kernel_start);
    __pe_data_rva   = ABSOLUTE(_etext - _kernel_start);
    __pe_entry_rva  = ABSOLUTE(efi_main - _kernel_start);
    __image_file_size = ABSOLUTE(_edata - _kernel_start);

    /DISCARD/ : {
        *(.comment) *(.note .note.*) *(.eh_frame*) *(.interp)
    }
}
