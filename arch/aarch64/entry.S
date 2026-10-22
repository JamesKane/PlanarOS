/*
 * arch/aarch64/entry.S -- boot entry for AArch64
 *
 * The firmware (U-Boot's booti, QEMU's -kernel, or the EFI stub in
 * efi.c) starts the boot CPU here with the MMU off, at EL2 (RK3588, CIX)
 * or EL1 (Q8B under Qualcomm's hypervisor, QEMU by default), with x0 =
 * the device tree or, from the EFI stub, a boot-information block
 * (kernel/bootinfo.h). The kernel always runs at EL1 (VQ11), so from EL2
 * it drops down first. The other CPUs wait in PSCI until V1 step 2
 * starts them.
 *
 * The image runs wherever it's loaded. At physical P it runs at the
 * direct-map address KERNEL_VOFFSET + P. It's linked as if P were 0, so
 * once the MMU is on, the entry code adds P to every address the image
 * holds (its R_AARCH64_RELATIVE relocations). The entry code builds one
 * set of boot tables and loads it in both TTBR0 and TTBR1, so it maps
 * the same physical memory at 0 and at the direct map:
 *
 *   L0[0], L0[1] -> L1: two tables, 1024 x 1 GiB blocks covering
 *                 physical 0..1 TiB (the CIX has RAM at 512 GiB), all
 *                 Device-nGnRE and execute-never, except
 *   L1[kg] -> L2: the GiB holding the kernel, as 2 MiB blocks: the
 *                 kernel image's are normal write-back memory, the rest
 *                 device.
 *
 * Then it turns the MMU on, relocates, and jumps to the high alias. arch_early_init()
 * turns TTBR0 off; arch_memory_init() makes the rest of RAM normal.
 */

/* RES1 bits (v8.0), less SPAN (bit 23): from v8.1, clearing it sets PAN
 * on every exception to EL1, so the kernel can't touch user memory but
 * through the unprivileged loads and stores of its copy routines. */
#define SCTLR_EL1_RES1  0x30500800      /* MMU, caches, alignment checks off */
/* ... | UCI | nTWE | UCT | DZE | I | C | M: EL0 may clean and invalidate
 * the data cache by address, and read CTR_EL0 for its line size, as a
 * driver whose device doesn't snoop must (arch/aarch64/user/cache.c);
 * a wfe in a spin loop and a dc zva in a memset don't trap (review
 * §10.8, conformance/userinsn.c). wfi still does. */
#define SCTLR_EL1_MMU   0x3454d805
#define HCR_EL2_RW      (1 << 31)       /* EL1 is AArch64 */
#define SPSR_EL1H_DAIF  0x3c5           /* EL1h, D A I F masked */
#define CPTR_EL2_RES1   0x22ff          /* RES1 only: no FP/SIMD, SVE or SME traps to EL2 */

/* MAIR_EL1 (docs §3.6): 0 Device-nGnRnE, 1 Device-nGnRE, 2 Normal-NC, 3 Normal-WB */
#define MAIR_VALUE      0xff440400

/* TCR_EL1: 48-bit VAs in both halves, 4 KiB granules, write-back walks,
 * inner shareable. IPS and AS are filled in from ID_AA64MMFR0_EL1. */
#define TCR_VALUE       0x00000000b5103510

/* Block descriptors: valid block, AttrIndx, SH, AF, PXN/UXN. */
#define BLOCK_DEVICE    0x0060000000000405  /* AttrIndx 1, AF, PXN, UXN */
#define BLOCK_NORMAL    0x004000000000070d  /* AttrIndx 3, SH inner, AF, UXN; global */
#define TABLE           0x3

#define KERNEL_VOFFSET  0xffff000000000000

    .section .text.head, "ax"
    .globl  _start
_start:
    /* arm64 Linux image header, so U-Boot's booti and QEMU accept it,
     * which is also a DOS header, so UEFI does. */
    .inst   0xfa405a4d              /* code0: "MZ", and a harmless ccmp */
    b       entry                   /* code1 */
    .quad   0x200000                /* text_offset */
    .quad   _image_size             /* image_size, including .bss and stack */
    .quad   0xa                     /* flags: LE, 4K pages, anywhere in RAM */
    .quad   0                       /* res2 */
    .quad   0                       /* res3 */
    .quad   0                       /* res4 */
    .ascii  "ARM\x64"               /* magic */
    .long   pe_header - _start      /* res5: the DOS header's PE offset */

/*
 * The PE/COFF header of an EFI application (PE32+): two sections, code
 * and data, at the addresses link.ld gives them, and no base relocations
 * (efi.c needs none; the kernel relocates itself, above). The values are
 * link.ld's.
 */
pe_header:
    .ascii  "PE\0\0"
    .short  0xaa64                  /* Machine: arm64 */
    .short  2                       /* NumberOfSections */
    .long   0                       /* TimeDateStamp */
    .long   0                       /* PointerToSymbolTable */
    .long   0                       /* NumberOfSymbols */
    .short  section_table - optional_header
    .short  0x206                   /* executable; no line numbers, no debug info */
optional_header:
    .short  0x20b                   /* PE32+ */
    .byte   0, 0                    /* linker version */
    .long   __pe_text_size          /* SizeOfCode */
    .long   __pe_data_size          /* SizeOfInitializedData */
    .long   __pe_bss_size           /* SizeOfUninitializedData */
    .long   __pe_entry_rva          /* AddressOfEntryPoint: efi_main */
    .long   __pe_text_rva           /* BaseOfCode */
    .quad   0                       /* ImageBase */
    .long   4096                    /* SectionAlignment */
    .long   4096                    /* FileAlignment */
    .short  0, 0, 0, 0, 0, 0        /* OS, image and subsystem versions */
    .long   0                       /* Win32VersionValue */
    .long   __pe_image_size         /* SizeOfImage */
    .long   __pe_text_rva           /* SizeOfHeaders: the first page */
    .long   0                       /* CheckSum */
    .short  10                      /* Subsystem: EFI application */
    .short  0x100                   /* DllCharacteristics: NX-compatible */
    .quad   0, 0, 0, 0              /* stack and heap sizes: unused */
    .long   0                       /* LoaderFlags */
    .long   6                       /* NumberOfRvaAndSizes */
    .quad   0, 0, 0, 0, 0, 0        /* export .. base relocation: none */
section_table:
    .ascii  ".text\0\0\0"
    .long   __pe_text_size          /* VirtualSize */
    .long   __pe_text_rva           /* VirtualAddress */
    .long   __pe_text_size          /* SizeOfRawData */
    .long   __pe_text_rva           /* PointerToRawData */
    .long   0, 0                    /* relocations, line numbers */
    .short  0, 0
    .long   0x60000020              /* code, execute, read */

    .ascii  ".data\0\0\0"
    .long   __pe_data_vsize         /* VirtualSize: with .bss */
    .long   __pe_data_rva
    .long   __pe_data_size
    .long   __pe_data_rva
    .long   0, 0
    .short  0, 0
    .long   0xc0000040              /* initialized data, read, write */

/*
 * EL2_TO_EL1 resume: if running at EL2, drop to EL1h at 'resume' with
 * interrupts masked. Clearing HCR_EL2 also clears E2H; firmware that
 * enters with VHE on (CIX) leaves it writable on the A720 (verify on the
 * board). EL1 gets the physical timer and counter, untrapped FP, and a
 * known SCTLR_EL1.
 *
 * Every EL2 control that can trap EL1 is written whole, not left as
 * found: on a core just powered on by PSCI they're UNKNOWN, and a trap
 * to an EL2 with no vectors hangs the core silently (three of the CIX's
 * A520s, at their first virtual timer access: CNTHCTL_EL2's FEAT_ECV
 * trap bits). VPIDR_EL2 and VMPIDR_EL2 get the CPU's own IDs. MDCR_EL2
 * gives EL1 every PMU counter; the fine-grained traps (FEAT_FGT) and
 * HCRX_EL2 (FEAT_HCX) are cleared where they exist; ICC_SRE_EL2 lets EL1
 * use the GIC's system registers. Uses x9 and x10.
 */
    .macro  EL2_TO_EL1 resume
    mrs     x9, CurrentEL
    lsr     x9, x9, #2
    cmp     x9, #2
    b.ne    \resume
    mov     x9, #HCR_EL2_RW
    msr     hcr_el2, x9
    mov     x9, #3                  /* EL1PCTEN, EL1PCEN; with FEAT_ECV, no */
    msr     cnthctl_el2, x9         /* trap of EL1's virtual timer (EL1TVT) */
    msr     cntvoff_el2, xzr
    mrs     x10, midr_el1           /* what EL1 reads as MIDR and MPIDR: the */
    msr     vpidr_el2, x10          /* CPU's own, which the kernel names CPUs */
    mrs     x10, mpidr_el1          /* by, not the firmware's leftovers */
    msr     vmpidr_el2, x10
    mov     x9, #CPTR_EL2_RES1
    msr     cptr_el2, x9
    msr     hstr_el2, xzr
    mov     x9, #0
    mrs     x10, id_aa64dfr0_el1
    ubfx    x10, x10, #8, #4        /* PMUVer: 0 none, 0xf IMPDEF */
    cbz     x10, 90f
    cmp     x10, #0xf
    b.eq    90f
    mrs     x9, pmcr_el0
    ubfx    x9, x9, #11, #5         /* N: HPMN, every counter EL1's */
90: msr     mdcr_el2, x9
    mrs     x10, id_aa64mmfr0_el1
    ubfx    x10, x10, #56, #4       /* FGT */
    cbz     x10, 91f
    msr     s3_4_c1_c1_4, xzr       /* HFGRTR_EL2 */
    msr     s3_4_c1_c1_5, xzr       /* HFGWTR_EL2 */
    msr     s3_4_c1_c1_6, xzr       /* HFGITR_EL2 */
    msr     s3_4_c3_c1_4, xzr       /* HDFGRTR_EL2 */
    msr     s3_4_c3_c1_5, xzr       /* HDFGWTR_EL2 */
    mrs     x10, id_aa64pfr0_el1
    ubfx    x10, x10, #44, #4       /* AMU */
    cbz     x10, 91f
    msr     s3_4_c3_c1_6, xzr       /* HAFGRTR_EL2 */
91: mrs     x10, id_aa64mmfr1_el1
    ubfx    x10, x10, #40, #4       /* HCX */
    cbz     x10, 92f
    msr     s3_4_c1_c2_2, xzr       /* HCRX_EL2 */
92: mrs     x10, id_aa64pfr0_el1
    ubfx    x10, x10, #24, #4       /* GIC: system registers */
    cbz     x10, 93f
    mov     x9, #0xf                /* ICC_SRE_EL2: Enable, DIB, DFB, SRE */
    msr     s3_4_c12_c9_5, x9
93: ldr     x9, =SCTLR_EL1_RES1
    msr     sctlr_el1, x9
    mov     x9, #SPSR_EL1H_DAIF
    msr     spsr_el2, x9
    adr     x9, \resume
    msr     elr_el2, x9
    isb
    eret
    .endm

entry:
    msr     daifset, #0xf
    EL2_TO_EL1 in_el1

in_el1:
    ldr     x9, =SCTLR_EL1_RES1
    msr     sctlr_el1, x9
    isb
    mov     x19, x0                 /* the device tree or boot block, for kmain */

    /* Clear .bss (physical addresses: the MMU is off). */
    adrp    x9, __bss_start
    add     x9, x9, :lo12:__bss_start
    adrp    x10, __bss_end
    add     x10, x10, :lo12:__bss_end
1:  cmp     x9, x10
    b.hs    2f
    str     xzr, [x9], #8
    b       1b
2:
    /* L1: every GiB a device block. */
    adrp    x0, boot_l1
    ldr     x1, =BLOCK_DEVICE
    mov     x2, #0
3:  orr     x3, x1, x2, lsl #30
    str     x3, [x0, x2, lsl #3]
    add     x2, x2, #1
    cmp     x2, #1024
    b.lo    3b

    /* L2 for the kernel's GiB (kg = _start >> 30): device 2 MiB blocks,
     * then normal ones over the image. */
    adrp    x4, _start              /* physical start of the image */
    lsr     x5, x4, #30             /* kg */
    adrp    x6, boot_l2
    mov     x2, #0
4:  lsl     x3, x5, #30
    orr     x3, x3, x2, lsl #21
    orr     x3, x3, x1
    str     x3, [x6, x2, lsl #3]
    add     x2, x2, #1
    cmp     x2, #512
    b.lo    4b

    ldr     x1, =BLOCK_NORMAL
    adrp    x7, _kernel_end
    add     x7, x7, :lo12:_kernel_end
    lsr     x8, x4, #21             /* first 2 MiB block of the image */
5:  lsl     x3, x8, #21
    orr     x3, x3, x1
    and     x2, x8, #511
    str     x3, [x6, x2, lsl #3]
    add     x8, x8, #1
    lsl     x9, x8, #21
    cmp     x9, x7
    b.lo    5b

    orr     x3, x6, #TABLE
    str     x3, [x0, x5, lsl #3]    /* L1[kg] -> L2 */
    adrp    x6, boot_l0
    orr     x3, x0, #TABLE
    str     x3, [x6]                /* L0[0] -> L1, 0..512 GiB */
    add     x3, x3, #4096
    str     x3, [x6, #8]            /* L0[1] -> L1, 512 GiB..1 TiB */

    /* Everything since the MMU went off was written past the caches.
     * Drop any lines a loader left for .bss (the boot tables are in it),
     * so none is read in place of what's in memory once the caches are
     * on. */
    mrs     x9, ctr_el0
    ubfx    x9, x9, #16, #4         /* DminLine: log2 words */
    mov     x10, #4
    lsl     x10, x10, x9            /* line size */
    adrp    x9, __bss_start
    add     x9, x9, :lo12:__bss_start
    adrp    x11, __bss_end
    add     x11, x11, :lo12:__bss_end
    sub     x12, x10, #1
    bic     x9, x9, x12
6:  dc      ivac, x9
    add     x9, x9, x10
    cmp     x9, x11
    b.lo    6b
    dsb     sy

    bl      mmu_enable

    /* Relocate, through the low alias: each place gets its addend plus
     * P, the physical start of the image (x4). r_offset is a link
     * address, KERNEL_VOFFSET + offset. link.ld's build check allows
     * only R_AARCH64_RELATIVE. */
    adrp    x9, __rela_start
    add     x9, x9, :lo12:__rela_start
    adrp    x10, __rela_end
    add     x10, x10, :lo12:__rela_end
7:  cmp     x9, x10
    b.hs    8f
    ldr     x11, [x9]               /* r_offset */
    ldr     x13, [x9, #16]          /* r_addend */
    add     x9, x9, #24
    and     x11, x11, #0xffffffffffff
    add     x13, x13, x4
    str     x13, [x11, x4]
    b       7b
8:
    /* Continue at the high alias, with the stack there too. */
    ldr     x9, =high
    br      x9
high:
    ldr     x9, =boot_stack_top
    mov     sp, x9
    /* kmain(boot CPU affinity, device tree or boot block). The RK3588 numbers its CPUs
     * in Aff1, so pass all of Aff2..Aff0. */
    mov     x1, x19
    mrs     x0, mpidr_el1
    and     x0, x0, #0xffffff
    bl      kmain
9:  wfe
    b       9b

/*
 * Turn the MMU on with the boot tables in both TTBRs (physical
 * addresses, no stack). The tables map physical memory at 0 as well as
 * high, so execution continues here. Uses x9-x11.
 */
mmu_enable:
    ldr     x9, =MAIR_VALUE
    msr     mair_el1, x9
    ldr     x9, =TCR_VALUE
    mrs     x10, id_aa64mmfr0_el1
    and     x11, x10, #0xf          /* PARange -> IPS */
    bfi     x9, x11, #32, #3
    ubfx    x11, x10, #4, #4        /* ASIDBits: 2 means 16-bit ASIDs */
    cmp     x11, #2
    b.ne    1f
    orr     x9, x9, #(1 << 36)      /* AS */
1:  msr     tcr_el1, x9
    adrp    x9, boot_l0
    msr     ttbr0_el1, x9
    msr     ttbr1_el1, x9
    isb
    tlbi    vmalle1
    dsb     nsh
    isb
    ldr     x9, =SCTLR_EL1_MMU
    msr     sctlr_el1, x9
    isb
    ret

/*
 * A secondary CPU, started by PSCI CPU_ON (arch_cpu_start), arrives here
 * with x0 = its struct cpu (a kernel address) and the MMU off, at EL2 or
 * EL1. The boot CPU has already built the tables.
 */
    .globl  secondary_entry
secondary_entry:
    msr     daifset, #0xf
    EL2_TO_EL1 secondary_el1
secondary_el1:
    ldr     x9, =SCTLR_EL1_RES1
    msr     sctlr_el1, x9
    isb
    mov     x19, x0
    bl      mmu_enable
    ldr     x9, =secondary_high
    br      x9
secondary_high:
    msr     tpidr_el1, x19          /* this_cpu() */
    ldr     x9, [x19]               /* struct cpu's stack_top */
    mov     sp, x9
    mov     x0, x19
    bl      kmain_secondary
8:  wfe
    b       8b

/*
 * efi_enter_kernel(info, image, size), from the EFI stub (efi.c) after
 * ExitBootServices. Write the copied image back to memory and drop it
 * from the caches, since entry runs with them off. Then turn the MMU off
 * at this EL and enter the image at its start, x0 = info, as any loader
 * would. The stub runs in UEFI's 1:1 map, so the instruction after the
 * MMU goes off is at the same address.
 */
    .text
    .globl  efi_enter_kernel
efi_enter_kernel:
    msr     daifset, #0xf
    mrs     x9, ctr_el0
    ubfx    x9, x9, #16, #4
    mov     x10, #4
    lsl     x10, x10, x9            /* line size */
    sub     x12, x10, #1
    bic     x9, x1, x12
    add     x11, x1, x2
1:  dc      civac, x9
    add     x9, x9, x10
    cmp     x9, x11
    b.lo    1b
    dsb     sy
    ic      iallu
    dsb     sy
    isb
    mrs     x9, CurrentEL
    cmp     x9, #(2 << 2)
    b.ne    2f
    mrs     x9, sctlr_el2
    bic     x9, x9, #1              /* M */
    bic     x9, x9, #4              /* C */
    msr     sctlr_el2, x9
    isb
    br      x1
2:  mrs     x9, sctlr_el1
    bic     x9, x9, #1
    bic     x9, x9, #4
    msr     sctlr_el1, x9
    isb
    br      x1

    .section .bss.boot_tables, "aw", @nobits
    .balign 4096
    .globl  boot_l0, boot_l1, boot_l2
boot_l0:
    .space  4096
boot_l1:
    .space  8192
boot_l2:
    .space  4096

    .section .bss.stack, "aw", @nobits
    .balign 16
boot_stack:
    .space  16384
boot_stack_top:
