/*
 * arch/rv64/entry.S -- boot entry for RV64 in S-mode
 *
 * The firmware (OpenSBI, then U-Boot's booti or QEMU's -kernel) starts the
 * boot hart here in S-mode with a0 = hart id and a1 = device tree, and the
 * MMU off. The other harts stay in the firmware until started with SBI
 * HSM (V1 step 2).
 *
 * The kernel is linked at its direct-map address (ARCH_DIRECT_MAP + load
 * address) but starts at its physical one; until the MMU is on, only
 * PC-relative addressing works. The entry code builds a boot page table
 * that maps the first DIRECT_MAP_GIB GiB of physical memory twice, at 0
 * and at the direct map, turns on Sv39 and jumps to the high alias.
 * arch_early_init() removes the low copy. From omega's entry.S.
 */

#define KERNEL_VOFFSET  0xffffffc000000000
#define DIRECT_MAP_GIB  16
#define ROOT_HIGH       256                 /* root index of KERNEL_VOFFSET */
#define PTE_KERNEL      0xef                /* V R W X G A D */
#define SATP_SV39       (8 << 60)

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
    /* Gigapages for physical 0 .. DIRECT_MAP_GIB GiB, at 0 and high. */
    la      t0, boot_root
    li      t1, 0
    li      t2, DIRECT_MAP_GIB
3:  slli    t3, t1, 28              /* PPN (i << 18) in PTE[53:10] */
    ori     t3, t3, PTE_KERNEL
    slli    t4, t1, 3
    add     t4, t4, t0
    sd      t3, 0(t4)
    li      t5, ROOT_HIGH * 8
    add     t4, t4, t5
    sd      t3, 0(t4)
    addi    t1, t1, 1
    blt     t1, t2, 3b

    srli    t0, t0, 12
    li      t1, SATP_SV39
    or      t0, t0, t1
    sfence.vma
    csrw    satp, t0
    sfence.vma

    /* Continue at the high alias, with the stack there too. */
    li      t1, KERNEL_VOFFSET
    la      t0, 4f
    add     t0, t0, t1
    jr      t0
4:  add     sp, sp, t1
    tail    kmain                   /* kmain(boot_hart, device_tree_pa) */

    /* The kernel's root page table, whose upper half every domain shares. */
    .section .bss.boot_root, "aw", @nobits
    .balign 4096
    .globl  boot_root
boot_root:
    .space  4096

    .section .bss.stack, "aw", @nobits
    .balign 16
boot_stack:
    .space  16384
boot_stack_top:
