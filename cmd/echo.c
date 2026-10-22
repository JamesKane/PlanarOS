/*
 * cmd/echo.c -- Echo TEXT/M,NOLINE/S: its words, to the console, one
 * space between each; with NOLINE no newline after
 */
#include <planar/dos.h>

static const char TEMPLATE[] = "TEXT/M,NOLINE/S";

int main(uint64_t arg)
{
    struct Con c;
    struct RDArgs ra = { 0 };
    int64_t a[2] = { 0 };
    char cwd[DOS_PATH];

    SplitArgs(arg, cwd, sizeof cwd, &ra.rda_Source);
    OpenCon(&c, 16);
    if (ReadArgs(TEMPLATE, a, &ra) != 0) {
        Printf(&c, "Echo: %s%s%s\n", ra.rda_Why, ra.rda_Why[0] ? ". Template: " : "", TEMPLATE);
        CloseCon(&c);
        return 1;
    }
    for (const char *const *w = (const char *const *)a[0]; w && *w; w++)
        Printf(&c, w[1] ? "%s " : "%s", *w);
    if (!a[1])
        PutStr(&c, "\n");
    FreeArgs(&ra);
    CloseCon(&c);
    return 0;
}
