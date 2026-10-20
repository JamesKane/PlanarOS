/*
 * arch/aarch64/entry.S -- boot entry for AArch64
 *
 * The firmware (U-Boot's booti, QEMU's -kernel, and later the EFI stub)
 * starts the boot CPU here with x0 = device tree and the MMU off, at EL2
 * (RK3588, CIX) or EL1 (Q8B under Qualcomm's hypervisor, QEMU by
 * default). The kernel always runs at EL1 (VQ11), so from EL2 it drops
 * down first. The other CPUs wait in PSCI until V1 step 2 starts them.
 *
 * The kernel is linked at its direct-map address (ARCH_DIRECT_MAP + load
 * address) but starts at its physical one. The entry code builds one set
 * of boot tables and loads it in both TTBR0 and TTBR1, so it maps the
 * same physical memory at 0 and at the direct map:
 *
 *   L0[0]  -> L1: 512 x 1 GiB blocks covering physical 0..512 GiB, all
 *                 Device-nGnRE and execute-never, except
 *   L1[kg] -> L2: the GiB holding the kernel, as 2 MiB blocks: the
 *                 kernel image's are normal write-back memory, the rest
 *                 device.
 *
 * Then it turns the MMU on and jumps to the high alias. arch_early_init()
 * turns TTBR0 off; arch_memory_init() makes the rest of RAM normal.
 */

#define SCTLR_EL1_RES1  0x30d00800      /* RES1 bits; MMU, caches, alignment checks off */
#define SCTLR_EL1_MMU   0x30d01805      /* RES1 | I | C | M */
#define HCR_EL2_RW      (1 << 31)       /* EL1 is AArch64 */
#define SPSR_EL1H_DAIF  0x3c5           /* EL1h, D A I F masked */
#define CPTR_EL2_RES1   0x33ff          /* no FP/SIMD or trace traps to EL2 */

/* MAIR_EL1 (docs §3.6): 0 Device-nGnRnE, 1 Device-nGnRE, 2 Normal-NC, 3 Normal-WB */
#define MAIR_VALUE      0xff440400

/* TCR_EL1: 48-bit VAs in both halves, 4 KiB granules, write-back walks,
 * inner shareable. IPS and AS are filled in from ID_AA64MMFR0_EL1. */
#define TCR_VALUE       0x00000000b5103510

/* Block descriptors: valid block, AttrIndx, SH, AF, PXN/UXN. */
#define BLOCK_DEVICE    0x0060000000000405  /* AttrIndx 1, AF, PXN, UXN */
#define BLOCK_NORMAL    0x000000000000070d  /* AttrIndx 3, SH inner, AF; global */
#define TABLE           0x3

#define KERNEL_VOFFSET  0xffff000000000000

    .section .text.head, "ax"
    .globl  _start
_start:
    /* arm64 Linux image header, so U-Boot's booti and QEMU accept it. */
    b       entry                   /* code0 */
    .word   0                       /* code1 */
    .quad   0x200000                /* text_offset */
    .quad   _image_size             /* image_size, including .bss and stack */
    .quad   0                       /* flags: LE, 4K pages unspecified, near DRAM base */
    .quad   0                       /* res2 */
    .quad   0                       /* res3 */
    .quad   0                       /* res4 */
    .ascii  "ARM\x64"               /* magic */
    .word   0                       /* res5 */

entry:
    msr     daifset, #0xf
    mrs     x9, CurrentEL
    lsr     x9, x9, #2
    cmp     x9, #2
    b.ne    in_el1

    /*
     * EL2 to EL1. Clearing HCR_EL2 also clears E2H; firmware that enters
     * with VHE on (CIX) leaves it writable on the A720 (verify on the
     * board). Give EL1 the physical timer and counter, untrapped FP, and
     * a known SCTLR_EL1.
     */
    mov     x9, #HCR_EL2_RW
    msr     hcr_el2, x9
    mrs     x9, cnthctl_el2
    orr     x9, x9, #3              /* EL1PCTEN, EL1PCEN */
    msr     cnthctl_el2, x9
    msr     cntvoff_el2, xzr
    mov     x9, #CPTR_EL2_RES1
    msr     cptr_el2, x9
    msr     hstr_el2, xzr
    ldr     x9, =SCTLR_EL1_RES1
    msr     sctlr_el1, x9
    mov     x9, #SPSR_EL1H_DAIF
    msr     spsr_el2, x9
    adr     x9, in_el1
    msr     elr_el2, x9
    isb
    eret

in_el1:
    ldr     x9, =SCTLR_EL1_RES1
    msr     sctlr_el1, x9
    isb
    mov     x19, x0                 /* the device tree, for kmain */

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
    cmp     x2, #512
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
    str     x3, [x6]                /* L0[0] -> L1 */

    /* Translation regime. */
    ldr     x9, =MAIR_VALUE
    msr     mair_el1, x9
    ldr     x9, =TCR_VALUE
    mrs     x10, id_aa64mmfr0_el1
    and     x11, x10, #0xf          /* PARange -> IPS */
    bfi     x9, x11, #32, #3
    ubfx    x11, x10, #4, #4        /* ASIDBits: 2 means 16-bit ASIDs */
    cmp     x11, #2
    b.ne    6f
    orr     x9, x9, #(1 << 36)      /* AS */
6:  msr     tcr_el1, x9
    msr     ttbr0_el1, x6
    msr     ttbr1_el1, x6
    isb
    tlbi    vmalle1
    dsb     nsh
    isb
    ldr     x9, =SCTLR_EL1_MMU
    msr     sctlr_el1, x9
    isb

    /* Continue at the high alias, with the stack there too. */
    ldr     x9, =high
    br      x9
high:
    ldr     x9, =boot_stack_top
    mov     sp, x9
    /* kmain(boot CPU affinity, device tree). The RK3588 numbers its CPUs
     * in Aff1, so pass all of Aff2..Aff0. */
    mov     x1, x19
    mrs     x0, mpidr_el1
    and     x0, x0, #0xffffff
    bl      kmain
7:  wfe
    b       7b

    .section .bss.boot_tables, "aw", @nobits
    .balign 4096
    .globl  boot_l0, boot_l1, boot_l2
boot_l0:
    .space  4096
boot_l1:
    .space  4096
boot_l2:
    .space  4096

    .section .bss.stack, "aw", @nobits
    .balign 16
boot_stack:
    .space  16384
boot_stack_top:
