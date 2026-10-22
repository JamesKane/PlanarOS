/*
 * servers/acpid/host.c -- acpid on the host, on captured tables
 *
 *   acpid-host [--out FILE] [--mem ADDR=FILE]... TABLE.dat...
 *
 * Loads the tables as acpid would (walk.c, osl.c, the same ACPICA) and
 * prints each device it finds, then the count (§9.6: how many took a
 * method to describe). --out writes the devices as acpid sends them to
 * hwdb, for hwdb-host's --namespace. --mem gives the bytes of memory
 * the AML reads at ADDR (a page of the firmware's, dumped on the board);
 * any other address the AML touches reads as all ones, as hwdb's
 * refusal would on Planar, and is listed.
 *
 * The tables lie in the host's memory, so this makes its own RSDP and
 * XSDT listing them, and points the FADT at the DSDT with no FACS.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "acpid.h"

constexpr unsigned TABLES = 64, MEMS = 8, TOUCHED = 64;

struct blob {
    uint8_t *p;
    uint64_t len, at;               /* at: where the AML thinks it is (--mem) */
};

static struct blob tables[TABLES + 3], mems[MEMS];
static unsigned ntables, nmems;
static uint64_t rsdp;
static uint64_t touched[TOUCHED];   /* pages refused, to list once */
static unsigned ntouched;

uint64_t acpid_rsdp(void)
{
    return rsdp;
}

uint64_t acpid_now(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000 + (uint64_t)t.tv_nsec;
}

void acpid_sleep(uint64_t)
{
}

void acpid_say(const char *line)
{
    printf("%s\n", line);
}

void acpid_say_acpica(const char *line)
{
    fprintf(stderr, "%s\n", line);
}

volatile void *acpid_map(uint64_t pa, uint64_t len, bool *refused)
{
    uint64_t page = pa & ~4095ULL;

    for (unsigned i = 0; i < ntables; i++)
        if (pa >= (uint64_t)tables[i].p && pa - (uint64_t)tables[i].p + len <= tables[i].len)
            return (volatile void *)pa;
    for (unsigned i = 0; i < nmems; i++)
        if (pa >= mems[i].at && pa - mems[i].at + len <= mems[i].len)
            return mems[i].p + (pa - mems[i].at);
    *refused = true;
    for (unsigned i = 0; i < ntouched; i++)
        if (touched[i] == page)
            return nullptr;
    if (ntouched < TOUCHED)
        touched[ntouched++] = page;
    return nullptr;
}

/* --- Tables. --- */

static uint8_t *read_file(const char *path, uint64_t *len)
{
    FILE *f = fopen(path, "rb");
    uint8_t *p;
    long n;

    if (!f || fseek(f, 0, SEEK_END) || (n = ftell(f)) < 0 || fseek(f, 0, SEEK_SET)) {
        fprintf(stderr, "acpid-host: can't read %s\n", path);
        exit(2);
    }
    p = calloc(1, (size_t)n + 16);
    if (!p || fread(p, 1, (size_t)n, f) != (size_t)n) {
        fprintf(stderr, "acpid-host: can't read %s\n", path);
        exit(2);
    }
    fclose(f);
    *len = (uint64_t)n;
    return p;
}

static void put64(uint8_t *p, uint64_t v)
{
    for (unsigned i = 0; i < 8; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static void put32(uint8_t *p, uint32_t v)
{
    for (unsigned i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static uint8_t sum(const uint8_t *p, uint64_t n)
{
    uint8_t s = 0;

    while (n--)
        s += *p++;
    return s;
}

static void add(uint8_t *p, uint64_t len)
{
    if (ntables == TABLES + 3) {
        fprintf(stderr, "acpid-host: too many tables\n");
        exit(2);
    }
    tables[ntables++] = (struct blob){ p, len, 0 };
}

static void build(char **files, unsigned n)
{
    uint8_t *fadt = nullptr, *dsdt = nullptr, *oem = nullptr, *xsdt, *r;
    uint64_t list[TABLES], fadt_len = 0;
    unsigned nlist = 0;

    for (unsigned i = 0; i < n; i++) {
        uint64_t len;
        uint8_t *p = read_file(files[i], &len);

        if (len < 36) {
            fprintf(stderr, "acpid-host: %s is too short for a table\n", files[i]);
            exit(2);
        }
        if (!memcmp(p, "XSDT", 4) || !memcmp(p, "RSDT", 4)) {
            oem = p;                /* only its OEM fields: the board, for hwdb */
            continue;
        }
        if (!memcmp(p, "FACS", 4))
            continue;
        add(p, len);
        if (!memcmp(p, "DSDT", 4))
            dsdt = p;
        else if (nlist < TABLES)
            list[nlist++] = (uint64_t)p;
        if (!memcmp(p, "FACP", 4)) {
            fadt = p;
            fadt_len = len;
        }
    }
    /* The FADT: the DSDT where it is here (a 64-bit address: ACPI 2.0's
     * X_DSDT), no FACS. */
    if (!fadt || !dsdt || fadt_len < 148) {
        fprintf(stderr, "acpid-host: no DSDT, or no FADT with X_DSDT\n");
        exit(2);
    }
    put32(fadt + 36, 0);
    put32(fadt + 40, 0);
    put64(fadt + 132, 0);
    put64(fadt + 140, (uint64_t)dsdt);
    fadt[9] = 0;
    fadt[9] = (uint8_t)-sum(fadt, fadt_len);

    xsdt = calloc(1, 36 + 8 * nlist);
    memcpy(xsdt, "XSDT", 4);
    put32(xsdt + 4, 36 + 8 * nlist);
    xsdt[8] = 1;
    memcpy(xsdt + 10, oem ? oem + 10 : (const uint8_t *)"PLANARHOSTTABLE", 18);
    for (unsigned i = 0; i < nlist; i++)
        put64(xsdt + 36 + 8 * i, list[i]);
    xsdt[9] = (uint8_t)-sum(xsdt, 36 + 8 * nlist);
    add(xsdt, 36 + 8 * nlist);

    r = calloc(1, 36);
    memcpy(r, "RSD PTR ", 8);
    memcpy(r + 9, xsdt + 10, 6);
    r[15] = 2;
    put32(r + 20, 36);
    put64(r + 24, (uint64_t)xsdt);
    r[8] = (uint8_t)-sum(r, 20);
    r[32] = (uint8_t)-sum(r, 36);
    add(r, 36);
    rsdp = (uint64_t)r;
}

/* --- The devices. --- */

static const char *str(const struct AcpidDevice *d, uint16_t at)
{
    return d->ad_Strings + at;
}

static void print_device(const struct AcpidDevice *d, void *ctx)
{
    static const char *const kinds[] = { "?", "mmio", "irq", "gpio", "bus", "window" };
    static const char *const bits[] = { "_HID", "_CID", "_UID", "_STA", "_CRS", "_DSD",
                                        "_INI", "power", "region", "refused", "failed" };
    FILE *out = ctx;

    printf("%s", str(d, d->ad_Path));
    for (unsigned i = 0; i < d->ad_NKeys; i++)
        printf("%s%s", i ? " " : " [", str(d, d->ad_Keys[i]));
    printf("%s sta %#x", d->ad_NKeys ? "]" : "", d->ad_Status);
    if (d->ad_Uid)
        printf(" uid %s", str(d, d->ad_Uid));
    if (d->ad_Adr != UINT64_MAX)
        printf(" adr %#llx", (unsigned long long)d->ad_Adr);
    if (d->ad_Methods) {
        printf(";");
        for (unsigned b = 0; b < sizeof bits / sizeof bits[0]; b++)
            if (d->ad_Methods & 1u << b)
                printf(" %s", bits[b]);
    }
    printf("\n");
    for (unsigned i = 0; i < d->ad_NRes; i++) {
        const struct AcpidRes *r = &d->ad_Res[i];

        bool range = r->ar_Kind == ACPID_R_MMIO || r->ar_Kind == ACPID_R_WINDOW;

        printf(range ? "  %s %#llx" : "  %s %llu", kinds[r->ar_Kind < 6 ? r->ar_Kind : 0],
               (unsigned long long)r->ar_Base);
        if (range)
            printf("+%#llx", (unsigned long long)r->ar_Size);
        if (r->ar_Kind == ACPID_R_GPIO && r->ar_Size > 1)
            printf(" (%llu pins)", (unsigned long long)r->ar_Size);
        if (r->ar_Flags)
            printf(" (%s%s%s%s)", r->ar_Flags & ACPID_RF_INT ? "int " : "",
                   r->ar_Flags & ACPID_RF_EDGE ? "edge" : "level",
                   r->ar_Flags & ACPID_RF_LOW ? " low" : "",
                   r->ar_Flags & ACPID_RF_SHARED ? " shared" : "");
        if (r->ar_Source)
            printf(" on %s", str(d, r->ar_Source));
        printf("\n");
    }
    if (d->ad_Dropped)
        printf("  %u resources not sent\n", d->ad_Dropped);
    if (out && fwrite(d, sizeof *d, 1, out) != 1) {
        fprintf(stderr, "acpid-host: can't write the devices\n");
        exit(2);
    }
}

int main(int argc, char **argv)
{
    FILE *out = nullptr;
    int i = 1;

    for (; i < argc && !strncmp(argv[i], "--", 2); i++) {
        if (!strcmp(argv[i], "--out") && i + 1 < argc) {
            if (!(out = fopen(argv[++i], "wb"))) {
                fprintf(stderr, "acpid-host: can't write %s\n", argv[i]);
                return 2;
            }
        } else if (!strcmp(argv[i], "--mem") && i + 1 < argc && nmems < MEMS) {
            char *eq = strchr(argv[++i], '=');

            if (!eq) {
                fprintf(stderr, "acpid-host: --mem ADDR=FILE\n");
                return 2;
            }
            *eq = '\0';
            mems[nmems].at = strtoull(argv[i], nullptr, 0);
            mems[nmems].p = read_file(eq + 1, &mems[nmems].len);
            nmems++;
        } else {
            fprintf(stderr, "usage: acpid-host [--out FILE] [--mem ADDR=FILE]... TABLE.dat...\n");
            return 2;
        }
    }
    if (i == argc) {
        fprintf(stderr, "usage: acpid-host [--out FILE] [--mem ADDR=FILE]... TABLE.dat...\n");
        return 2;
    }
    build(argv + i, (unsigned)(argc - i));
    if (!acpid_load())
        return 1;
    acpid_walk(print_device, out);
    acpid_say_count();
    for (unsigned k = 0; k < ntouched; k++)
        printf("acpid: the AML touched %#llx, which the host hasn't got\n",
               (unsigned long long)touched[k]);
    if (out)
        fclose(out);
    return 0;
}
