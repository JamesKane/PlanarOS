/*
 * cmd/touch.c -- Touch FILE/M/A: make each file, empty, if it isn't there
 */
#include <planar/dos.h>
#include <planar/syscalls.h>
#include <planar/utility.h>

static const char TEMPLATE[] = "FILE/M/A";

int main(uint64_t arg)
{
    struct Con c;
    struct Dos d;
    struct RDArgs ra = { 0 };
    int64_t a[1] = { 0 }, f;
    char cwd[DOS_PATH], path[DOS_PATH];
    int failed = 0;

    SplitArgs(arg, cwd, sizeof cwd, &ra.rda_Source);
    OpenCon(&c, 16);
    if (ReadArgs(TEMPLATE, a, &ra) != 0) {
        Printf(&c, "Touch: %s%s%s\n", ra.rda_Why, ra.rda_Why[0] ? ". Template: " : "", TEMPLATE);
        CloseCon(&c);
        return 1;
    }
    if (OpenDos(&d, 17) != 0) {
        PutStr(&c, "Touch: no dos\n");
        FreeArgs(&ra);
        CloseCon(&c);
        return 1;
    }
    for (const char *const *n = (const char *const *)a[0]; *n; n++) {
        SNPrintf(path, sizeof path, "%s", cwd);
        if (!AddPart(path, *n, sizeof path) ||
            (f = OpenFile(&d, path, DOSF_WRITE | DOSF_CREATE, nullptr)) < 0) {
            Printf(&c, "Touch: can't make %s\n", path);
            failed = 1;
            continue;
        }
        CloseFile(&d, (uint32_t)f);
    }
    CloseDos(&d);
    FreeArgs(&ra);
    CloseCon(&c);
    return failed;
}
