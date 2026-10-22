/*
 * cmd/list.c -- List DIR/M,FILES/S,DIRS/S,QUICK/S: directories' entries,
 * their sizes, and how many (the current directory if none is named).
 * FILES shows only files, DIRS only directories, QUICK only names.
 */
#include <planar/dos.h>
#include <planar/syscalls.h>
#include <planar/utility.h>

static const char TEMPLATE[] = "DIR/M,FILES/S,DIRS/S,QUICK/S";

enum { A_DIR, A_FILES, A_DIRS, A_QUICK, A_COUNT };

/* One directory: 0, or 1 if it isn't one. */
static int list(struct Con *c, struct Dos *d, const char *path, const int64_t *a)
{
    struct DosEntry e;
    uint32_t cursor = 0, files = 0, dirs = 0;
    uint64_t bytes = 0;
    int64_t f;

    if ((f = OpenFile(d, path, 0, &e)) < 0 || !e.de_Dir) {
        if (f >= 0)
            CloseFile(d, (uint32_t)f);
        Printf(c, "List: not a directory: %s\n", path);
        return 1;
    }
    Printf(c, "Directory %s\n", path);
    while (ExamineNext(d, (uint32_t)f, &cursor, &e) == 0) {
        if (e.de_Dir ? a[A_FILES] && !a[A_DIRS] : a[A_DIRS] && !a[A_FILES])
            continue;
        if (a[A_QUICK])
            Printf(c, "  %s\n", e.de_Name);
        else if (e.de_Dir)
            Printf(c, "  %s (dir)\n", e.de_Name);
        else
            Printf(c, "  %s  %lu\n", e.de_Name, e.de_Size);
        if (e.de_Dir) {
            dirs++;
        } else {
            files++;
            bytes += e.de_Size;
        }
    }
    Printf(c, "%u files, %u directories, %lu bytes\n", files, dirs, bytes);
    CloseFile(d, (uint32_t)f);
    return 0;
}

int main(uint64_t arg)
{
    static const char *const here[] = { "", nullptr };
    struct Con c;
    struct Dos d;
    struct RDArgs ra = { 0 };
    int64_t a[A_COUNT] = { (int64_t)here };
    char cwd[DOS_PATH], path[DOS_PATH];
    int failed = 0;

    SplitArgs(arg, cwd, sizeof cwd, &ra.rda_Source);
    OpenCon(&c, 16);
    if (ReadArgs(TEMPLATE, a, &ra) != 0) {
        Printf(&c, "List: %s%s%s\n", ra.rda_Why, ra.rda_Why[0] ? ". Template: " : "", TEMPLATE);
        CloseCon(&c);
        return 1;
    }
    if (OpenDos(&d, 17) != 0) {
        PutStr(&c, "List: no dos\n");
        FreeArgs(&ra);
        CloseCon(&c);
        return 1;
    }
    for (const char *const *n = (const char *const *)a[A_DIR]; *n; n++) {
        SNPrintf(path, sizeof path, "%s", cwd);
        if (!AddPart(path, *n, sizeof path)) {
            Printf(&c, "List: too long: %s\n", *n);
            failed = 1;
            continue;
        }
        failed |= list(&c, &d, path, a);
    }
    CloseDos(&d);
    FreeArgs(&ra);
    CloseCon(&c);
    return failed;
}
