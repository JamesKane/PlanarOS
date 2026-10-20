/*
 * arch/rv64/entry.S -- boot entry for RV64 in S-mode
 *
 * The firmware (OpenSBI, then U-Boot's booti or QEMU's -kernel) starts the
 * boot hart here in S-mode with a0 = hart id and a1 = device tree, and the
 * MMU off. The other harts stay in the firmware until started with SBI
 * HSM (V1).
 *
 * V0 runs at its physical load address with the MMU off: the memory
 * layout (identity or global-VA, VQ3) is chosen in V1.
 */

    .option norvc

    .section .text.head, "ax"
    .globl  _start
_start:
    /* RISC-V Linux image header, so U-Boot's booti accepts the image. */
    j       entry                   /* code0 */
    .word   0                       /* code1 */
    .dword  0x200000                /* text_offset */
    .dword  _image_size             /* image_size, including .bss and stack */
    .dword  0                       /* flags: little-endian */
    .word   2                       /* version 0.2 */
    .word   0                       /* res1 */
    .dword  0                       /* res2 */
    .ascii  "RISCV\0\0\0"           /* magic (deprecated) */
    .ascii  "RSC\x05"               /* magic2 */
    .word   0                       /* res3 */

entry:
    csrw    sie, zero
    la      sp, boot_stack_top

    /* Clear .bss. Only t registers are used, so a0/a1 survive for kmain. */
    la      t0, __bss_start
    la      t1, __bss_end
1:  bgeu    t0, t1, 2f
    sd      zero, 0(t0)
    addi    t0, t0, 8
    j       1b
2:
    tail    kmain                   /* kmain(boot_hart, device_tree) */

    .section .bss.stack, "aw", @nobits
    .balign 16
boot_stack:
    .space  16384
boot_stack_top:
