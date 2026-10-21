/*
 * conformance/fwreader.c -- reads the firmware's description, if it may
 *
 * Started by conformance/leaser.c with the address of the DTB or the
 * RSDP. Exits 0 if the first bytes are a DTB's magic or "RSD PTR ";
 * faults if the tables weren't granted to it (GrantFirmware).
 */
#include <stdint.h>

int main(uint64_t at)
{
    const volatile uint8_t *p = (const volatile uint8_t *)at;

    if (p[0] == 0xd0 && p[1] == 0x0d && p[2] == 0xfe && p[3] == 0xed)
        return 0;
    return p[0] == 'R' && p[1] == 'S' && p[2] == 'D' && p[7] == ' ' ? 0 : 1;
}
