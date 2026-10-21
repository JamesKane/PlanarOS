/*
 * conformance/dostest.c -- the dos server and dos.library (v4-plan step 6)
 *
 * hwdb's test mode starts dos, then this. It lists the volumes and SYS:'s
 * EFI/planar, reads the files tools/mkdisk.py put there (on a board's own
 * disk they aren't, and are skipped), and runs C:hello from the disk,
 * which must exit 7, as the built-in one does. On a writable volume (a
 * test disk) it then makes a directory and a file across many clusters,
 * reads it back, and removes both. Exit 0 if all of it held.
 */
#include <stddef.h>

#include <planar/dos.h>
#include <planar/syscalls.h>

void *memset(void *dst, int c, size_t n);

constexpr int64_t SIG_DOS = 16;
constexpr int64_t SIG_CHILD = 17;
constexpr int64_t SIG_NAP = 18;
constexpr uint64_t MS = 1000000;

static unsigned failures;
static uint8_t big[150000], back[150000];

struct line {
    char text[200];
    unsigned len;
};

static void put(struct line *l, const char *s)
{
    while (*s && l->len < sizeof l->text - 1)
        l->text[l->len++] = *s++;
    l->text[l->len] = '\0';
}

static void put_dec(struct line *l, uint64_t v)
{
    char d[24];
    unsigned n = 0;

    do
        d[n++] = (char)('0' + v % 10);
    while ((v /= 10) && n < sizeof d);
    while (n && l->len < sizeof l->text - 1)
        l->text[l->len++] = d[--n];
    l->text[l->len] = '\0';
}

static void check(bool ok, const char *what)
{
    struct line l = { 0 };

    if (ok)
        return;
    put(&l, "dostest: failed: ");
    put(&l, what);
    KLog(l.text);
    failures++;
}

static bool same(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return *a == *b;
}

static void nap(uint64_t ms)
{
    ArmTimer(CurrentTime() + ms * MS, SIG_NAP);
    Wait(1ULL << SIG_NAP);
}

int main(void)
{
    struct dos d;
    struct DosEntry e;
    struct line l = { 0 };
    char text[64];
    int64_t f, r;
    uint32_t cursor = 0, n = 0;
    bool writable = false, test_disk = false;
    struct ProcessInfo pi;

    /* dos may still be mounting. */
    for (uint64_t since = CurrentTime(); (r = dos_open(&d, SIG_DOS)) == ERR_NOTFOUND;) {
        if (CurrentTime() - since > 5000 * MS)
            break;
        nap(2);
    }
    if (r) {
        KLog("dostest: no dos server");
        return 1;
    }

    put(&l, "dostest: volumes");
    for (uint32_t i = 0; dos_volume(&d, i, &e) == 0; i++) {
        put(&l, i ? ", " : " ");
        put(&l, e.de_Name);
        put(&l, e.de_Dir ? " (writable, " : " (read only, ");
        put_dec(&l, e.de_Size >> 20);
        put(&l, " MiB free)");
    }
    KLog(l.text);
    check(dos_stat(&d, "SYS:EFI/planar", &e) == 0 && e.de_Dir, "no SYS:EFI/planar");

    /* SYS:EFI/planar, listed. */
    l = (struct line){ 0 };
    put(&l, "dostest: SYS:EFI/planar holds");
    if ((f = dos_file(&d, "SYS:EFI/planar", 0, nullptr)) >= 0) {
        while (dos_next(&d, (uint32_t)f, &cursor, &e) == 0) {
            put(&l, n++ ? ", " : " ");
            put(&l, e.de_Name);
            if (e.de_Dir)
                put(&l, "/");
            test_disk |= same(e.de_Name, "hello.txt");
        }
        dos_close_file(&d, (uint32_t)f);
    }
    KLog(l.text);

    /* The test disk's files: a short name, and a long one. */
    if (test_disk) {
        f = dos_file(&d, "SYS:EFI/planar/hello.txt", 0, &e);
        check(f >= 0 && e.de_Size == 30, "hello.txt isn't there");
        r = f >= 0 ? dos_read(&d, (uint32_t)f, 0, text, sizeof text) : -1;
        check(r == 30 && text[0] == 'H' && text[29] == '\n', "hello.txt reads wrong");
        dos_close_file(&d, (uint32_t)f);
        check(dos_stat(&d, "SYS:efi/planar/a file with a long name.txt", &e) == 0 &&
                  same(e.de_Name, "A file with a long name.txt"),
              "the long name isn't found");
    }

    /* A program from the disk. */
    r = dos_run(&d, "C:hello", 0, SIG_CHILD, 0);
    check(r >= 0, "C:hello wouldn't load");
    if (r >= 0) {
        uint64_t since = CurrentTime();

        while (QueryProcess((Handle)r, &pi) == 0 && pi.pi_State < PROCS_EXITED &&
               CurrentTime() - since < 3000 * MS)
            nap(1);
        check(pi.pi_State == PROCS_EXITED && pi.pi_ExitCode == 7, "C:hello didn't exit 7");
        CloseHandle((Handle)r);
    }
    if (test_disk)
        check(dos_run(&d, "SYS:EFI/planar/hello.txt", 0, SIG_CHILD, 0) == ERR_BADARG,
              "a text file was taken for a program");

    /* Writing, where it may. */
    for (uint32_t i = 0; dos_volume(&d, i, &e) == 0; i++)
        writable |= e.de_Dir && test_disk;
    if (writable) {
        for (unsigned i = 0; i < sizeof big; i++)
            big[i] = (uint8_t)(i * 13 + (i >> 11));
        check(dos_mkdir(&d, "SYS:EFI/planar/tmp") == 0, "mkdir");
        f = dos_file(&d, "SYS:EFI/planar/tmp/data.bin", DOSF_WRITE | DOSF_CREATE, nullptr);
        check(f >= 0, "create");
        if (f >= 0) {
            check(dos_write(&d, (uint32_t)f, 0, big, sizeof big) == sizeof big, "write");
            memset(back, 0, sizeof back);
            check(dos_read(&d, (uint32_t)f, 0, back, sizeof back) == sizeof back, "read back");
            for (unsigned i = 0; i < sizeof big; i++)
                if (back[i] != big[i]) {
                    check(false, "what was read back differs");
                    break;
                }
            check(dos_remove(&d, "SYS:EFI/planar/tmp/data.bin") == ERR_BUSY, "removed while open");
            dos_close_file(&d, (uint32_t)f);
        }
        check(dos_stat(&d, "SYS:EFI/planar/tmp/data.bin", &e) == 0 && e.de_Size == sizeof big,
              "its size");
        check(dos_remove(&d, "SYS:EFI/planar/tmp") == ERR_EXISTS, "a full directory removed");
        check(dos_remove(&d, "SYS:EFI/planar/tmp/data.bin") == 0, "remove the file");
        check(dos_remove(&d, "SYS:EFI/planar/tmp") == 0, "remove the directory");
    } else {
        check(dos_mkdir(&d, "SYS:EFI/planar/tmp") == ERR_RIGHTS, "a read-only volume written");
    }
    dos_close(&d);

    l = (struct line){ 0 };
    put(&l, failures ? "dostest: FAILED: " : "dostest: ok: ");
    put(&l, "volumes listed, ");
    put(&l, test_disk ? "files read by short and long names, " : "no test files (a board's disk), ");
    put(&l, "C:hello loaded from it and run");
    put(&l, writable ? ", a 150 KB file written, read back and removed" : ", nothing written");
    KLog(l.text);
    return failures ? 1 : 0;
}
