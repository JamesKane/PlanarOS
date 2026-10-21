/*
 * arch/aarch64/user/elf.c -- what this machine's programs say they're for
 * (dos.library's loader)
 */
#include <stdint.h>

const uint16_t user_elf_machine = 183;      /* EM_AARCH64 */
