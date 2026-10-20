/*
 * arch/host/start.c -- hosted build entry
 *
 * The host process stands in for the boot CPU. A device tree blob can be
 * given on the command line (for example one dumped from QEMU or a
 * board), to exercise the firmware-description code on the host.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

[[noreturn]] void kmain(uint64_t boot_cpu, const void *firmware_data);

static void *read_file(const char *path)
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
    }
    fclose(f);
    return buf;
}

int main(int argc, char **argv)
{
    const void *fdt = nullptr;

    if (argc > 1 && !(fdt = read_file(argv[1]))) {
        fprintf(stderr, "vectra: can't read %s\n", argv[1]);
        return 1;
    }
    kmain(0, fdt);
}
