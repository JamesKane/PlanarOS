/*
 * arch/host/start.c -- hosted build entry
 *
 * The host process stands in for the boot CPU. A firmware description
 * can be given on the command line, to exercise the code that reads it
 * on the host:
 *
 *   planar [DTB] [--acpi TABLE...] [--args 'WORDS'] [--boot-cpu HEX]
 *
 * DTB is a device tree blob (dumped from QEMU or a board). --acpi takes
 * raw ACPI tables as a board's /dev/mem or acpidump gives them, such as
 * the .dat files in docs/boards/cix-p1/acpi: they're listed in a new XSDT, under a
 * new RSDP, with the OEM IDs of the board's own XSDT if it's among them.
 * Either way the kernel gets a boot-information block, as from the EFI
 * stub, with --args as its command line.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bootinfo.h"

[[noreturn]] void kmain(uint64_t boot_cpu, uint64_t firmware_data);

constexpr int MAX_TABLES = 64;

static void *read_file(const char *path, long *size)
{
    FILE *f = fopen(path, "rb");
    void *buf = nullptr;
    long n;

    if (!f)
        return nullptr;
    if (fseek(f, 0, SEEK_END) == 0 && (n = ftell(f)) > 0 && fseek(f, 0, SEEK_SET) == 0) {
        buf = malloc((size_t)n);
        if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
            free(buf);
            buf = nullptr;
        }
        if (size)
            *size = n;
    }
    fclose(f);
    return buf;
}

static void put_le(uint8_t *p, uint64_t v, int bytes)
{
    for (int i = 0; i < bytes; i++)
        p[i] = (uint8_t)(v >> 8 * i);
}

static uint8_t checksum(const uint8_t *p, size_t n)
{
    uint8_t sum = 0;

    while (n--)
        sum += *p++;
    return (uint8_t)-sum;
}

/* An RSDP and an XSDT listing 'n' tables; nullptr if out of memory. */
static uint8_t *make_rsdp(uint8_t *const *tables, int n, const uint8_t *board_xsdt)
{
    size_t xlen = 36 + 8 * (size_t)n;
    uint8_t *xsdt = calloc(1, xlen), *rsdp = calloc(1, 36);

    if (!xsdt || !rsdp)
        return nullptr;
    memcpy(xsdt, "XSDT", 4);
    put_le(xsdt + 4, xlen, 4);
    xsdt[8] = 1;
    memcpy(xsdt + 10, board_xsdt ? board_xsdt + 10 : (const uint8_t *)"PLANARHOSTED  ", 14);
    for (int i = 0; i < n; i++)
        put_le(xsdt + 36 + 8 * i, (uint64_t)(uintptr_t)tables[i], 8);
    xsdt[9] = checksum(xsdt, xlen);

    memcpy(rsdp, "RSD PTR ", 8);
    memcpy(rsdp + 9, xsdt + 10, 6);
    rsdp[15] = 2;
    put_le(rsdp + 20, 36, 4);
    put_le(rsdp + 24, (uint64_t)(uintptr_t)xsdt, 8);
    rsdp[8] = checksum(rsdp, 20);
    rsdp[32] = checksum(rsdp, 36);
    return rsdp;
}

int main(int argc, char **argv)
{
    static struct boot_info bi = { .magic = BOOT_INFO_MAGIC };
    uint8_t *tables[MAX_TABLES];
    const uint8_t *board_xsdt = nullptr;
    uint64_t boot_cpu = 0;
    int ntables = 0;
    bool acpi = false;

    for (int i = 1; i < argc; i++) {
        long size = 0;
        uint8_t *t;

        if (strcmp(argv[i], "--acpi") == 0) {
            acpi = true;
        } else if (strcmp(argv[i], "--args") == 0 && i + 1 < argc) {
            snprintf(bi.cmdline, sizeof bi.cmdline, "%s", argv[++i]);
        } else if (strcmp(argv[i], "--boot-cpu") == 0 && i + 1 < argc) {
            boot_cpu = strtoull(argv[++i], nullptr, 16);
        } else if (!(t = read_file(argv[i], &size))) {
            fprintf(stderr, "planar: can't read %s\n", argv[i]);
            return 1;
        } else if (!acpi) {
            bi.dtb = (uint64_t)(uintptr_t)t;
        } else if (size >= 36 && memcmp(t, "XSDT", 4) == 0) {
            board_xsdt = t;             /* its list is the board's addresses */
        } else if (size >= 36 && memcmp(t, "DSDT", 4) != 0 && memcmp(t, "FACS", 4) != 0) {
            if (ntables == MAX_TABLES) {
                fprintf(stderr, "planar: more than %d tables\n", MAX_TABLES);
                return 1;
            }
            tables[ntables++] = t;      /* the FADT's, not the XSDT's */
        }
    }
    if (acpi && !(bi.rsdp = (uint64_t)(uintptr_t)make_rsdp(tables, ntables, board_xsdt))) {
        fprintf(stderr, "planar: out of memory\n");
        return 1;
    }
    kmain(boot_cpu, bi.dtb || bi.rsdp || bi.cmdline[0] ? (uint64_t)(uintptr_t)&bi : 0);
}
