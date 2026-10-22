/*
 * cmd/delete.c -- Delete FILE/M/A,QUIET/S: remove files, or empty
 * directories, saying which unless QUIET
 */
#include <planar/dos.h>
#include <planar/syscalls.h>
#include <planar/utility.h>

static const char TEMPLATE[] = "FILE/M/A,QUIET/S";

int main(uint64_t arg)
{
    struct Con c;
    struct Dos d;
    struct RDArgs ra = { 0 };
    int64_t a[2] = { 0 }, e;
    char cwd[DOS_PATH], path[DOS_PATH];
    int failed = 0;

    SplitArgs(arg, cwd, sizeof cwd, &ra.rda_Source);
    OpenCon(&c, 16);
    if (ReadArgs(TEMPLATE, a, &ra) != 0) {
        Printf(&c, "Delete: %s%s%s\n", ra.rda_Why, ra.rda_Why[0] ? ". Template: " : "", TEMPLATE);
        CloseCon(&c);
        return 1;
    }
    if (OpenDos(&d, 17) != 0) {
        PutStr(&c, "Delete: no dos\n");
        FreeArgs(&ra);
        CloseCon(&c);
        return 1;
    }
    for (const char *const *f = (const char *const *)a[0]; *f; f++) {
        SNPrintf(path, sizeof path, "%s", cwd);
        if (!AddPart(path, *f, sizeof path))
            e = ERR_BADARG;
        else
            e = DeletePath(&d, path);
        if (e == 0 && !a[1])
            Printf(&c, "%s  Deleted\n", path);
        else if (e != 0)
            Printf(&c, "Delete: %s %s\n", e == ERR_EXISTS ? "not empty:" : "can't delete", path);
        failed |= e != 0;
    }
    CloseDos(&d);
    FreeArgs(&ra);
    CloseCon(&c);
    return failed;
}
