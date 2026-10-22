/*
 * cmd/info.c -- Info VOLUME: the volumes, or the one named, how much is
 * free on each, and whether it can be written
 */
#include <planar/dos.h>
#include <planar/syscalls.h>
#include <planar/utility.h>

static const char TEMPLATE[] = "VOLUME";

int main(uint64_t arg)
{
    struct Con c;
    struct Dos d;
    struct DosEntry e;
    struct RDArgs ra = { 0 };
    int64_t a[1] = { 0 };
    char cwd[DOS_PATH];
    struct Str want = { 0 };
    unsigned shown = 0;

    SplitArgs(arg, cwd, sizeof cwd, &ra.rda_Source);
    OpenCon(&c, 16);
    if (ReadArgs(TEMPLATE, a, &ra) != 0) {
        Printf(&c, "Info: %s%s%s\n", ra.rda_Why, ra.rda_Why[0] ? ". Template: " : "", TEMPLATE);
        CloseCon(&c);
        return 1;
    }
    if (a[0]) {                     /* "SYS" or "SYS:" */
        want = StrFromC((const char *)a[0]);
        if (want.st_Len && want.st_Data[want.st_Len - 1] == ':')
            want.st_Len--;
    }
    if (OpenDos(&d, 17) != 0) {
        PutStr(&c, "Info: no dos\n");
        FreeArgs(&ra);
        CloseCon(&c);
        return 1;
    }
    PutStr(&c, "Volumes:\n");
    for (uint32_t i = 0; NextVolume(&d, i, &e) == 0; i++) {
        if (a[0] && !StrSameNoCase(StrFromC(e.de_Name), want))
            continue;
        Printf(&c, "  %s: %lu MiB free, %s\n", e.de_Name, e.de_Size >> 20,
               e.de_Dir ? "read/write" : "read only");
        shown++;
    }
    if (a[0] && !shown)
        Printf(&c, "Info: no volume %s\n", (const char *)a[0]);
    CloseDos(&d);
    FreeArgs(&ra);
    CloseCon(&c);
    return a[0] && !shown;
}
