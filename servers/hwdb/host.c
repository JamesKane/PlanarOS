/*
 * servers/hwdb/host.c -- hwdb's graph on the host, for testing (§11.5)
 *
 *   hwdb-host [--detail] --dtb FILE
 *   hwdb-host [--detail] --acpi TABLE.dat...
 *   hwdb-host --ties
 *
 * Builds the graph from a captured device tree or ACPI tables, applies
 * the quirks and the match tables, then binds as the server would, every
 * driver succeeding at once: the next ready node, then its consumers.
 * Prints the report. --ties checks the match tables (make check).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hwdb.h"

static void print(void *ctx, const char *line)
{
    fprintf(ctx, "%s\n", line);
}

static void *slurp(const char *path, uint32_t *len)
{
    FILE *f = fopen(path, "rb");
    long n;
    void *p;

    if (!f || fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
        perror(path);
        exit(2);
    }
    p = malloc(n ? (size_t)n : 1);
    if (!p || fread(p, 1, (size_t)n, f) != (size_t)n) {
        perror(path);
        exit(2);
    }
    fclose(f);
    *len = (uint32_t)n;
    return p;
}

int main(int argc, char **argv)
{
    static uint8_t space[8 << 20];
    struct hw_arena arena = { space, sizeof space, 0 };
    struct hw_graph g;
    struct hw_table tables[32];
    unsigned ntables = 0;
    bool detail = false, ok;
    const char *dtb = nullptr;
    int i = 1;

    if (argc == 2 && strcmp(argv[1], "--ties") == 0) {
        unsigned ties = hw_match_ties(hw_drivers, hw_ndrivers, print, stderr);

        printf("hwdb: %u match tables, %u ties\n", hw_ndrivers, ties);
        return ties ? 1 : 0;
    }
    for (int k = 1; k < argc; k++)
        if (strcmp(argv[k], "--detail") == 0) {
            detail = true;
            for (int m = k; m + 1 < argc; m++)
                argv[m] = argv[m + 1];
            argc--;
        }
    if (i + 1 < argc && strcmp(argv[i], "--dtb") == 0) {
        dtb = argv[i + 1];
    } else if (i + 1 < argc && strcmp(argv[i], "--acpi") == 0) {
        for (i++; i < argc && ntables < 32; i++)
            tables[ntables].data = slurp(argv[i], &tables[ntables].len), ntables++;
    } else {
        fprintf(stderr, "usage: hwdb-host [--detail] --dtb FILE | --acpi TABLE...\n"
                        "       hwdb-host --ties\n");
        return 2;
    }

    hw_graph_init(&g, &arena, 4096);
    if (dtb) {
        uint32_t len;
        void *blob = slurp(dtb, &len);

        ok = hw_from_fdt(&g, blob, len);
    } else {
        ok = hw_from_acpi(&g, tables, ntables);
    }
    if (!ok) {
        fprintf(stderr, "hwdb-host: the source is unusable%s\n", g.full ? " (out of space)" : "");
        return 1;
    }
    hw_prepare(&g);
    hw_bind_all(&g);
    hw_report(&g, detail, print, stdout);
    if (g.full) {
        fprintf(stderr, "hwdb-host: out of space\n");
        return 1;
    }
    return 0;
}
