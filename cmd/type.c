/*
 * cmd/type.c -- Type FROM/A/M,NUMBER/S: files' text, to the console;
 * with NUMBER each line numbered
 */
#include <planar/dos.h>
#include <planar/syscalls.h>
#include <planar/utility.h>

static const char TEMPLATE[] = "FROM/A/M,NUMBER/S";

static char buf[4096];

/* One file: 0, or 1 if it couldn't be read. */
static int type(struct Con *c, struct Dos *d, const char *path, bool number)
{
    struct DosEntry e;
    int64_t f, n;
    uint64_t off = 0, line = 1;
    bool start = true;

    if ((f = OpenFile(d, path, 0, &e)) < 0 || e.de_Dir) {
        if (f >= 0)
            CloseFile(d, (uint32_t)f);
        Printf(c, "Type: can't open %s\n", path);
        return 1;
    }
    while ((n = ReadFile(d, (uint32_t)f, off, buf, sizeof buf)) > 0) {
        off += (uint64_t)n;
        if (!number) {
            WriteCon(c, buf, (uint32_t)n);
            continue;
        }
        for (int64_t i = 0, from = 0; i < n; i++) {
            if (start)
                Printf(c, "%4lu ", line++);
            start = buf[i] == '\n';
            if (start || i == n - 1) {
                WriteCon(c, buf + from, (uint32_t)(i + 1 - from));
                from = i + 1;
            }
        }
    }
    CloseFile(d, (uint32_t)f);
    return n < 0;
}

int main(uint64_t arg)
{
    struct Con c;
    struct Dos d;
    struct RDArgs ra = { 0 };
    int64_t a[2] = { 0 };
    char cwd[DOS_PATH], path[DOS_PATH];
    int failed = 0;

    SplitArgs(arg, cwd, sizeof cwd, &ra.rda_Source);
    OpenCon(&c, 16);
    if (ReadArgs(TEMPLATE, a, &ra) != 0) {
        Printf(&c, "Type: %s%s%s\n", ra.rda_Why, ra.rda_Why[0] ? ". Template: " : "", TEMPLATE);
        CloseCon(&c);
        return 1;
    }
    if (OpenDos(&d, 17) != 0) {
        PutStr(&c, "Type: no dos\n");
        FreeArgs(&ra);
        CloseCon(&c);
        return 1;
    }
    for (const char *const *f = (const char *const *)a[0]; *f; f++) {
        SNPrintf(path, sizeof path, "%s", cwd);
        if (!AddPart(path, *f, sizeof path)) {
            Printf(&c, "Type: too long: %s\n", *f);
            failed = 1;
            continue;
        }
        failed |= type(&c, &d, path, a[1]);
    }
    CloseDos(&d);
    FreeArgs(&ra);
    CloseCon(&c);
    return failed;
}
