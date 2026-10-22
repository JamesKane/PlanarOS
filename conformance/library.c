/*
 * conformance/library.c -- exec.library and utility.library through
 * their interfaces (V6.4)
 *
 * IExec from the kernel at the start, and its header; OpenLibrary and
 * GetInterface, with a version too new, a name unknown and an interface
 * that isn't there refused. Calls through both tables: memory by the
 * table's CopyMem and SetMem at every length to 300 and each alignment,
 * AllocVec sharing the program's own pool, CreateTask starting a task
 * in the program's trampoline, SNPrintf, GetTagData and a Span read.
 * Then a second process, which must start with the same IExec and map
 * the same library pages, shared: one copy. And a third, which writes
 * to the table, and must fault: the region is read-only. Exits 0, or the
 * step that failed; the second exits with its IExec.
 */
#include <planar/exec.h>
#include <planar/utility.h>

#include "log.h"

#define CHECK(step, cond) \
    if (!(cond)) \
        return (step)

constexpr uint64_t WRITE = 1ULL << 63;         /* the third's argument */
constexpr uint64_t DONE = 1ULL << 20;

static struct UtilityIFace *IUtility;
static uint8_t src[320], dst[320], want[320];
static thread_local uint64_t mine = 7;
static volatile uint64_t task_saw;
static Handle first;

static bool same(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return *a == *b;
}

static bool equal(const uint8_t *a, const uint8_t *b, unsigned n)
{
    while (n--)
        if (*a++ != *b++)
            return false;
    return true;
}

static int64_t library_pages(void)
{
    struct ProcessInfo pi;

    return QueryProcess(0, &pi) == 0 ? (int64_t)pi.pi_LibraryPages : -1;
}

static void in_task(uint64_t arg)
{
    task_saw = arg + mine;              /* its own thread_local, so made */
    Signal(first, DONE);
}

static int interfaces(void)
{
    struct Library *exec, *util;

    CHECK(10, IExec && same(IExec->Data.if_Name, "main") && IExec->Data.if_Version == 2);
    CHECK(11, IExec->Data.if_Size == sizeof(struct ExecIFace));
    exec = IExec->Data.if_Library;
    CHECK(12, same(exec->lib_Name, "exec.library") && exec->lib_Version == 6);
    CHECK(13, IExec->OpenLibrary("exec.library", 6) == exec);
    CHECK(14, !IExec->OpenLibrary("exec.library", 7) && !IExec->OpenLibrary("no.library", 0) &&
                  !IExec->OpenLibrary(nullptr, 0));
    CHECK(15, (util = IExec->OpenLibrary("utility.library", 6)));
    IUtility = (struct UtilityIFace *)IExec->GetInterface(util, "main", 1);
    CHECK(16, IUtility && IUtility->Data.if_Library == util &&
                  IUtility->Data.if_Size == sizeof(struct UtilityIFace));
    CHECK(17, !IExec->GetInterface(util, "main", 2) && !IExec->GetInterface(util, "other", 1));
    CHECK(18, IExec->GetInterface(exec, "main", 1) == &IExec->Data);
    IExec->DropInterface(&IUtility->Data);      /* counted by nothing in V6 */
    IExec->CloseLibrary(util);
    return 0;
}

static int calls(void)
{
    static const struct TagItem tags[] = {
        { TAG_USER + 1, 5 }, { TAG_USER + 2, 6 }, { TAG_DONE, 0 },
    };
    static const uint8_t be[] = { 0x12, 0x34, 0x56, 0x78 };
    uint32_t w = 0;
    char line[32];
    uint8_t *v;

    for (unsigned i = 0; i < sizeof src; i++)
        src[i] = (uint8_t)(i * 7 + 1);
    for (unsigned n = 0; n <= 300; n++)
        for (unsigned a = 0; a < 8; a++) {
            SetMem(dst, 0xee, sizeof dst);
            SetMem(want, 0xee, sizeof want);
            memcpy(want + a, src + (7 - a), n);
            IExec->CopyMem(src + (7 - a), dst + a, n);
            CHECK(20, equal(dst, want, sizeof dst));
            IExec->SetMem(dst + a, (uint8_t)n, n);
            SetMem(want + a, (uint8_t)n, n);
            CHECK(21, equal(dst, want, sizeof dst));
        }
    IExec->CopyMem(src, src + 3, 200);          /* overlapping */
    CHECK(22, src[3] == 1 && src[202] == (uint8_t)(199 * 7 + 1));
    IExec->CopyMemQuick(src, dst, 64);
    CHECK(23, equal(src, dst, 64));

    /* One pool for the process: allocated by the program's AllocVec,
     * freed by the library's, and the other way. */
    CHECK(24, (v = IExec->AllocVec(100, AREAF_CLEAR)) && v[0] == 0 && v[99] == 0);
    FreeVec(v);
    CHECK(25, (v = AllocVec(100, 0)));
    IExec->FreeVec(v);
    CHECK(26, IExec->AllocVec(100, 0) == v);    /* the same block, from the same list */
    FreeVec(v);

    first = FindTask(nullptr);
    CHECK(27, IExec->CreateTask(in_task, 100) > 0);
    Wait(DONE);
    CHECK(28, task_saw == 107);

    CHECK(29, IUtility->SNPrintf(line, sizeof line, "%s %lu", "via", 64UL) == 6 &&
                  same(line, "via 64"));
    CHECK(30, IUtility->GetTagData(TAG_USER + 2, 0, tags) == 6 && IUtility->ToUpper('q') == 'Q');
    CHECK(31, IUtility->SpanReadU32BE((struct Span){ be, 4 }, 0, &w) && w == 0x12345678 &&
                  !IUtility->SpanReadU32BE((struct Span){ be, 4 }, 1, &w));
    CHECK(32, IUtility->StrSame(IUtility->StrTrim(STR("  x ")), STR("x")));
    return 0;
}

/* The second process: the same IExec, and the region mapped, shared. */
static int second(uint64_t pages)
{
    struct AreaInfo ai;

    if (library_pages() != (int64_t)pages || QueryArea(IExec, &ai) != 0)
        return 1;
    if (ai.ai_Area != 0 || ai.ai_Sharers < 2 || ai.ai_Rights != (AREAR_READ | AREAR_EXEC) ||
        ai.ai_Size != pages * 4096)
        return 2;
    if (interfaces() || calls())
        return 3;
    ExitProcess((int64_t)(uintptr_t)IExec);
}

static int64_t run(uint64_t arg, struct ProcessInfo *pi)
{
    int64_t sig = AllocSignal(-1), h;

    if (sig < 0 || (h = CreateProcess("library", arg, sig, 0)) < 0)
        return -1;
    Wait(1ULL << sig);
    QueryProcess((Handle)h, pi);
    CloseHandle((Handle)h);
    FreeSignal(sig);
    return 0;
}

/* What a call costs through the table, against the program's own. */
static void costs(void)
{
    constexpr unsigned N = 10000;
    uint64_t t0, own, table, tag;
    void *v;

    t0 = CurrentTime();
    for (unsigned i = 0; i < N; i++) {
        v = AllocVec(64, 0);
        FreeVec(v);
    }
    own = (CurrentTime() - t0) / N;
    t0 = CurrentTime();
    for (unsigned i = 0; i < N; i++) {
        v = IExec->AllocVec(64, 0);
        IExec->FreeVec(v);
    }
    table = (CurrentTime() - t0) / N;
    t0 = CurrentTime();
    for (unsigned i = 0; i < N; i++)
        (void)IUtility->GetTagData(TAG_USER, 0, nullptr);
    tag = (CurrentTime() - t0) / N;
    KPrintF("library: AllocVec and FreeVec %lu ns linked in, %lu ns through IExec; an empty "
            "GetTagData through IUtility %lu ns", own, table, tag);
}

int main(uint64_t arg)
{
    struct ProcessInfo pi;
    struct AreaInfo ai;
    int64_t pages;
    int r;

    if (arg & WRITE) {
        *(volatile uint64_t *)&IExec->AllocVec = 0;     /* must fault */
        return 1;
    }
    if (arg)
        return second(arg);
    if ((r = interfaces()) || (r = calls()))
        return r;
    pages = library_pages();
    CHECK(40, pages > 0 && QueryArea(IExec, &ai) == 0 && ai.ai_Area == 0 &&
                  ai.ai_Size == (uint64_t)pages * 4096);
    CHECK(41, run((uint64_t)pages, &pi) == 0 && pi.pi_State == PROCS_EXITED);
    CHECK(42, pi.pi_ExitCode == (int64_t)(uintptr_t)IExec);
    CHECK(43, run(WRITE, &pi) == 0 && pi.pi_State == PROCS_FAULTED &&
                  pi.pi_Fault == PFAULT_WRITE &&
                  pi.pi_FaultAddress == (uint64_t)(uintptr_t)&IExec->AllocVec);
    KPrintF("library: exec.library %u.%u and utility.library through their tables in two "
            "processes, %ld pages shared, the table read-only",
            IExec->Data.if_Library->lib_Version, IExec->Data.if_Library->lib_Revision, pages);
    costs();
    return 0;
}
