/*
 * arch/aarch64/entry.S -- boot entry for AArch64
 *
 * The firmware (U-Boot's booti, QEMU's -kernel, and later the EFI stub)
 * starts the boot CPU here with x0 = device tree and the MMU off, at EL2
 * (RK3588, CIX) or EL1 (Q8B under Qualcomm's hypervisor, QEMU by
 * default). The kernel always runs at EL1 (VQ11), so from EL2 it drops
 * down first. The other CPUs wait in PSCI until V1 starts them.
 *
 * V0 runs at its physical load address with the MMU off: the memory
 * layout (identity or global-VA, VQ3) is chosen in V1.
 */

#define SCTLR_EL1_RES1  0x30d00800      /* RES1 bits; MMU, caches, alignment checks off */
#define HCR_EL2_RW      (1 << 31)       /* EL1 is AArch64 */
#define SPSR_EL1H_DAIF  0x3c5           /* EL1h, D A I F masked */
#define CPTR_EL2_RES1   0x33ff          /* no FP/SIMD or trace traps to EL2 */

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
    adrp    x9, boot_stack_top
    add     x9, x9, :lo12:boot_stack_top
    mov     sp, x9

    /* Clear .bss. x0 (the device tree) must survive for kmain. */
    adrp    x9, __bss_start
    add     x9, x9, :lo12:__bss_start
    adrp    x10, __bss_end
    add     x10, x10, :lo12:__bss_end
1:  cmp     x9, x10
    b.hs    2f
    str     xzr, [x9], #8
    b       1b
2:
    /* kmain(boot CPU affinity, device tree). The RK3588 numbers its CPUs
     * in Aff1, so pass all of Aff2..Aff0. */
    mov     x1, x0
    mrs     x0, mpidr_el1
    and     x0, x0, #0xffffff
    bl      kmain
3:  wfe
    b       3b

    .section .bss.stack, "aw", @nobits
    .balign 16
boot_stack:
    .space  16384
boot_stack_top:
