/*
 * servers/dos/host.c -- fat.c on the host, on a disk image (make dos-test)
 *
 *   fat-host IMAGE ls PATH | cat PATH | put FILE PATH | mkdir PATH | rm PATH | df
 *   fat-host IMAGE test
 *
 * The volume is the image's first GPT partition. "test" makes and
 * removes files and directories (enough to grow a directory and a file
 * across clusters), reading each back; then fsck_msdos can check what's
 * left (the Makefile runs it where it's installed).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <planar/endian.h>
#include <planar/syscalls.h>

#include "fat.h"

static FILE *img;
static uint64_t part_start;

static int64_t rd(void *, uint64_t s, uint32_t n, void *buf)
{
    return fseek(img, (long)((part_start + s) * 512), SEEK_SET) == 0 &&
                   fread(buf, 512, n, img) == n
               ? 0
               : ERR_BADARG;
}

static int64_t wr(void *, uint64_t s, uint32_t n, const void *buf)
{
    return fseek(img, (long)((part_start + s) * 512), SEEK_SET) == 0 &&
                   fwrite(buf, 512, n, img) == n
               ? 0
               : ERR_BADARG;
}

static int failures;

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: failed: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static void test(struct fat_vol *v)
{
    struct fat_node n, f;
    char buf[256], name[64];
    static uint8_t big[70000], back[70000];
    uint32_t free0 = fat_free(v), cursor = 0;
    int count = 0;

    CHECK(fat_lookup(v, "EFI/planar/hello.txt", &n) == 0 && !n.dir);
    CHECK(fat_read(v, &n, 0, buf, sizeof buf) == 30 && memcmp(buf, "Hello from the EFI", 18) == 0);
    CHECK(fat_lookup(v, "efi/PLANAR/a file with a long name.txt", &n) == 0);  /* any case */
    CHECK(strcmp(n.name, "A file with a long name.txt") == 0 && n.lfn == 3);
    CHECK(fat_lookup(v, "EFI/nothing", &n) == ERR_NOTFOUND);
    CHECK(fat_lookup(v, "EFI/planar/hello.txt/x", &n) == ERR_NOTFOUND);

    /* A directory of many files: it grows past its first cluster. */
    CHECK(fat_create(v, "EFI/planar/many", true, &n) == 0 && n.dir);
    CHECK(fat_create(v, "EFI/planar/many", true, &n) == ERR_EXISTS);
    for (int i = 0; i < 40; i++) {
        snprintf(name, sizeof name, "EFI/planar/many/f%d.txt", i);
        CHECK(fat_create(v, name, false, &f) == 0);
        snprintf(buf, sizeof buf, "file %d\n", i);
        CHECK(fat_write(v, &f, 0, buf, (uint32_t)strlen(buf)) == (int64_t)strlen(buf));
    }
    CHECK(fat_lookup(v, "EFI/planar/many", &n) == 0);
    while (fat_next(v, &n, &cursor, &f) == 0)
        count++;
    CHECK(count == 40);
    CHECK(fat_lookup(v, "EFI/planar/many/f39.txt", &f) == 0 && f.size == 8);
    CHECK(fat_read(v, &f, 0, buf, 64) == 8 && memcmp(buf, "file 39\n", 8) == 0);

    /* A file across clusters, written in pieces out of order. */
    for (unsigned i = 0; i < sizeof big; i++)
        big[i] = (uint8_t)(i * 7 + (i >> 9));
    CHECK(fat_create(v, "EFI/planar/big.bin", false, &f) == 0);
    CHECK(fat_write(v, &f, 1000, big + 1000, 60000) == 60000);
    CHECK(fat_write(v, &f, 0, big, 1000) == 1000);
    CHECK(fat_write(v, &f, 61000, big + 61000, sizeof big - 61000) == (int64_t)sizeof big - 61000);
    CHECK(fat_lookup(v, "EFI/planar/big.bin", &f) == 0 && f.size == sizeof big);
    CHECK(fat_read(v, &f, 0, back, sizeof back) == (int64_t)sizeof back &&
          memcmp(big, back, sizeof big) == 0);
    CHECK(fat_read(v, &f, 4097, back, 3) == 3 && memcmp(back, big + 4097, 3) == 0);
    CHECK(fat_truncate(v, &f, 5000) == 0 && f.size == 5000);
    CHECK(fat_read(v, &f, 0, back, sizeof back) == 5000 && memcmp(big, back, 5000) == 0);

    /* Names: mixed case or too long for 8.3 is a long name, with a short
     * alias (V8 step 7), found again by it in any case. */
    CHECK(fat_create(v, "EFI/planar/Mixed.txt", false, &f) == 0 && f.lfn == 1);
    CHECK(fat_create(v, "EFI/planar/mixed.TXT", false, &f) == ERR_EXISTS);
    CHECK(fat_create(v, "EFI/planar/toolongname.txt", false, &f) == 0 && f.lfn == 2);
    CHECK(fat_create(v, "EFI/planar/toolongname.text", false, &f) == 0);
    CHECK(fat_lookup(v, "EFI/planar/TOOLONGNAME.TEXT", &f) == 0 && strcmp(f.name, "toolongname.text") == 0);
    CHECK(fat_create(v, "EFI/planar/Env-Archive", true, &n) == 0 && n.dir);
    CHECK(fat_create(v, "EFI/planar/env-archive/input.prefs", false, &f) == 0);
    CHECK(fat_write(v, &f, 0, "prefs", 5) == 5);
    CHECK(fat_lookup(v, "EFI/planar/Env-Archive/Input.Prefs", &f) == 0 && f.size == 5 &&
          strcmp(f.name, "input.prefs") == 0);
    CHECK(fat_create(v, "EFI/planar/bad?.txt", false, &f) == ERR_BADARG);
    CHECK(fat_create(v, "EFI/planar/a b.", false, &f) == ERR_BADARG);
    CHECK(fat_create(v, "EFI/nodir/x", false, &f) == ERR_NOTFOUND);
    CHECK(fat_remove(v, "EFI/planar/Mixed.txt") == 0 && fat_lookup(v, "EFI/planar/mixed.txt", &f) == ERR_NOTFOUND);
    CHECK(fat_remove(v, "EFI/planar/toolongname.txt") == 0);
    CHECK(fat_remove(v, "EFI/planar/toolongname.text") == 0);
    CHECK(fat_remove(v, "EFI/planar/Env-Archive/input.prefs") == 0);
    CHECK(fat_remove(v, "EFI/planar/Env-Archive") == 0);

    /* Removing: a full directory refuses; then it all goes. */
    CHECK(fat_remove(v, "EFI/planar/many") == ERR_EXISTS);
    for (int i = 0; i < 40; i++) {
        snprintf(name, sizeof name, "EFI/planar/many/f%d.txt", i);
        CHECK(fat_remove(v, name) == 0);
    }
    CHECK(fat_remove(v, "EFI/planar/many") == 0);
    CHECK(fat_remove(v, "EFI/planar/big.bin") == 0);
    CHECK(fat_lookup(v, "EFI/planar/many", &n) == ERR_NOTFOUND);
    /* The directory 'planar' keeps the cluster it grew; nothing else stays. */
    CHECK(fat_free(v) + 1 >= free0 && fat_free(v) <= free0);

    /* What fsck then checks: a file and a directory left on purpose. */
    CHECK(fat_create(v, "EFI/planar/made", true, &n) == 0);
    CHECK(fat_create(v, "EFI/planar/made/note.txt", false, &f) == 0);
    CHECK(fat_write(v, &f, 0, "written by fat.c\n", 17) == 17);
    CHECK(fat_create(v, "EFI/planar/made/A Long Name.prefs", false, &f) == 0);
    CHECK(fat_write(v, &f, 0, "long\n", 5) == 5);
    printf("fat-host: %d checks failed; %u clusters free\n", failures, fat_free(v));
}

int main(int argc, char **argv)
{
    static uint8_t cache[FAT_MEM], sec[512], ents[16384];
    struct fat_io io = { nullptr, rd, wr };
    struct fat_vol v;
    struct fat_node n;
    int64_t e;

    if (argc < 3 || !(img = fopen(argv[1], argc > 3 && strcmp(argv[2], "ls") && strcmp(argv[2], "cat") ? "r+b" : (strcmp(argv[2], "test") == 0 ? "r+b" : "rb")))) {
        fprintf(stderr, "usage: fat-host IMAGE ls|cat|put|mkdir|rm|df|test ...\n");
        return 2;
    }
    /* The first GPT partition. */
    if (fseek(img, 512, SEEK_SET) || fread(sec, 1, 512, img) != 512 || memcmp(sec, "EFI PART", 8) ||
        fseek(img, (long)(get_le64(sec + 72) * 512), SEEK_SET) || fread(ents, 1, sizeof ents, img) != sizeof ents) {
        fprintf(stderr, "fat-host: no GPT\n");
        return 2;
    }
    part_start = get_le64(ents + 32);
    if ((e = fat_mount(&v, &io, get_le64(ents + 40) - part_start + 1, cache))) {
        fprintf(stderr, "fat-host: not FAT32 (%lld)\n", (long long)e);
        return 1;
    }
    if (strcmp(argv[2], "test") == 0) {
        test(&v);
        return failures ? 1 : 0;
    }
    if (strcmp(argv[2], "df") == 0) {
        printf("%s: %u clusters of %u bytes, %u free\n", v.label, v.clusters, v.spc * v.bps,
               fat_free(&v));
        return 0;
    }
    if (argc < 4)
        return 2;
    if (strcmp(argv[2], "ls") == 0) {
        uint32_t cursor = 0;
        struct fat_node c;

        if ((e = fat_lookup(&v, argv[3], &n)))
            return fprintf(stderr, "%lld\n", (long long)e), 1;
        while (fat_next(&v, &n, &cursor, &c) == 0)
            printf("%s%s %u\n", c.name, c.dir ? "/" : "", c.size);
        return 0;
    }
    if (strcmp(argv[2], "cat") == 0) {
        char buf[4096];
        uint64_t off = 0;

        if ((e = fat_lookup(&v, argv[3], &n)))
            return fprintf(stderr, "%lld\n", (long long)e), 1;
        while ((e = fat_read(&v, &n, off, buf, sizeof buf)) > 0) {
            fwrite(buf, 1, (size_t)e, stdout);
            off += (uint64_t)e;
        }
        return e < 0;
    }
    if (strcmp(argv[2], "mkdir") == 0)
        return fat_create(&v, argv[3], true, &n) != 0;
    if (strcmp(argv[2], "rm") == 0)
        return fat_remove(&v, argv[3]) != 0;
    if (strcmp(argv[2], "put") == 0 && argc == 5) {
        FILE *f = fopen(argv[3], "rb");
        char buf[4096];
        size_t got;
        uint64_t off = 0;

        if (!f || fat_create(&v, argv[4], false, &n) != 0)
            return 1;
        while ((got = fread(buf, 1, sizeof buf, f)) > 0) {
            if (fat_write(&v, &n, off, buf, (uint32_t)got) != (int64_t)got)
                return 1;
            off += got;
        }
        return 0;
    }
    return 2;
}
