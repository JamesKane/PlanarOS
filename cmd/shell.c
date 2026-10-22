/*
 * cmd/shell.c -- the Shell (v4-plan step 7): commands from C:, run as
 * processes, their output on the console
 *
 *   Shell              read lines from the serial console (decision 1)
 *   Shell FROM FILE    run FILE's lines, then end: 0 if all succeeded
 *
 * Its template is "FROM" (ReadArgs). Started from a Shell, it begins in
 * that one's current directory. A line's first word is a built-in (CD,
 * Help, EndShell) or a command, found in C: unless it names its volume
 * or directory; the rest of the line is its arguments, and it's given
 * the current directory with them (SplitArgs). The Shell waits for it
 * and says how it ended if not well.
 * Input is the console's (GrantConsole, by hwdb) and a USB keyboard's,
 * from input's ring (planar/devices/input.h), which the Shell takes when
 * input is there and again after it restarts. Both are polled every
 * 10 ms, echoed, with backspace.
 */
#include <stddef.h>

#include <planar/devices/input.h>
#include <planar/dos.h>
#include <planar/lease.h>
#include <planar/syscalls.h>
#include <planar/utility.h>

void *memset(void *dst, int c, size_t n);

constexpr int64_t SIG_CON = 16;
constexpr int64_t SIG_DOS = 17;
constexpr int64_t SIG_CHILD = 18;
constexpr int64_t SIG_TICK = 19;
constexpr int64_t SIG_INPUT = 20;
constexpr uint64_t MS = 1000000;
constexpr unsigned LINE = 200;

static struct Con con;
static struct Dos dos;
static char cwd[DOS_PATH] = "SYS:";
static unsigned number = 1;             /* the Amiga's CLI number, for the prompt */

static void say(const char *s)
{
    PutStr(&con, s);
}

static bool word_is(const char *w, unsigned n, const char *name)
{
    unsigned i = 0;

    for (; i < n && name[i]; i++) {
        char a = w[i] >= 'A' && w[i] <= 'Z' ? (char)(w[i] + 32) : w[i];

        if (a != name[i])
            return false;
    }
    return i == n && !name[i];
}

static void nap(uint64_t ms)
{
    ArmTimer(CurrentTime() + ms * MS, SIG_TICK);
    Wait(1ULL << SIG_TICK);
}

/* CD DIR: a directory that exists becomes the current one. "/" is a
 * step up, ":" the volume's root (AddPart). */
static int cd(const char *line)
{
    struct RDArgs ra = { .rda_Source = line };
    int64_t a[1] = { 0 };
    char path[DOS_PATH];
    struct DosEntry e;
    int r = 0;

    if (ReadArgs("DIR", a, &ra) != 0) {
        Printf(&con, "CD: %s%s\n", ra.rda_Why, ra.rda_Why[0] ? ". Template: DIR" : "DIR");
        return 1;
    }
    if (!a[0]) {
        Printf(&con, "%s\n", cwd);
    } else {
        SNPrintf(path, sizeof path, "%s", cwd);
        if (!AddPart(path, (const char *)a[0], sizeof path) || ExaminePath(&dos, path, &e) != 0 ||
            !e.de_Dir) {
            Printf(&con, "CD: not a directory: %s\n", path);
            r = 1;
        } else {
            SNPrintf(cwd, sizeof cwd, "%s", path);
        }
    }
    FreeArgs(&ra);
    return r;
}

/* One line: 0 if it went well. */
static int run(const char *line)
{
    char path[DOS_PATH], name[DOS_PATH], args[DOS_ARGS];
    unsigned n = 0, k = 0;
    const char *rest;
    struct ProcessInfo pi;
    bool place = false;
    int64_t p;

    while (*line == ' ')
        line++;
    while (line[n] && line[n] != ' ')
        n++;
    if (n == 0 || line[0] == ';')
        return 0;                       /* empty, or a comment */
    rest = line + n;
    while (*rest == ' ')
        rest++;
    if (word_is(line, n, "cd"))
        return cd(rest);
    if (word_is(line, n, "help")) {
        say("Built in: CD [DIR], Help, EndShell. Commands are in C:.\n");
        return 0;
    }
    if (word_is(line, n, "endshell") || word_is(line, n, "endcli"))
        return -1;

    /* A command: C:name, or the path it gives. */
    for (unsigned i = 0; i < n && i < DOS_PATH - 1; i++) {
        name[i] = line[i];
        place |= line[i] == ':' || line[i] == '/';
        name[i + 1] = '\0';
    }
    SNPrintf(path, sizeof path, "%s", place ? cwd : "C:");
    if (!AddPart(path, name, sizeof path)) {
        Printf(&con, "Too long: %s\n", name);
        return 1;
    }
    for (const char *s = cwd; *s && k < DOS_ARGS - 2; s++)
        args[k++] = *s;
    args[k++] = '\n';
    for (const char *s = rest; *s && k < DOS_ARGS - 1; s++)
        args[k++] = *s;
    args[k] = '\0';
    if ((p = RunCommand(&dos, path, args, SIG_CHILD, 0)) < 0) {
        say(p == ERR_NOTFOUND ? "Unknown command " : "Can't run ");
        say(name);
        say("\n");
        return 1;
    }
    while (QueryProcess((Handle)p, &pi) == 0 && pi.pi_State < PROCS_EXITED)
        Wait(1ULL << SIG_CHILD);
    CloseHandle((Handle)p);
    if (pi.pi_State == PROCS_EXITED && pi.pi_ExitCode == 0)
        return 0;
    if (pi.pi_State == PROCS_EXITED)
        Printf(&con, "%s failed returncode %ld\n", name, pi.pi_ExitCode);
    else
        Printf(&con, "%s faulted\n", name);
    return 1;
}

/* --- Keys from input. --- */

static struct {
    bool up;
    struct MsgPort mp;
    const struct InputRing *ring;
    uint64_t tail, retry;
} keys;

static void keys_take(void)
{
    struct InputMsg *m;

    if (CurrentTime() < keys.retry)
        return;
    keys.retry = CurrentTime() + 500 * MS;
    if (ConnectPort(&keys.mp, "input", SIG_INPUT, 2, 2 * sizeof *m) != 0)
        return;
    if (!(m = AllocMsg(&keys.mp, sizeof *m))) {
        ClosePort(&keys.mp);
        return;
    }
    memset(m, 0, sizeof *m);
    m->im_Msg.mn_Length = sizeof *m;
    m->im_Op = INPUT_OPEN;
    m->im_Task = FindTask(nullptr);
    if (DoMsg(&keys.mp, &m->im_Msg) != 0 || m->im_Msg.mn_Error != 0) {
        ClosePort(&keys.mp);        /* another has the ring, or input went */
        return;
    }
    keys.ring = (const struct InputRing *)m->im_Ring;
    keys.tail = keys.ring->ir_Head;
    keys.up = true;
}

/* What's been typed on a keyboard since last time: up to 'n' characters. */
static int64_t keys_read(char *buf, unsigned n)
{
    unsigned got = 0;

    if (keys.up && PortGone(&keys.mp)) {
        ClosePort(&keys.mp);
        keys.up = false;
    }
    if (!keys.up) {
        keys_take();
        return 0;
    }
    while (got < n) {
        uint64_t head = __atomic_load_n(&keys.ring->ir_Head, __ATOMIC_ACQUIRE);
        struct InputEvent e;

        if (head - keys.tail > INPUT_RING)
            keys.tail = head - INPUT_RING;
        if (keys.tail == head)
            break;
        e = keys.ring->ir_Event[keys.tail % INPUT_RING];
        if (__atomic_load_n(&keys.ring->ir_Head, __ATOMIC_ACQUIRE) - keys.tail > INPUT_RING)
            continue;               /* overwritten as it was read */
        keys.tail++;
        if (e.ie_Class == IECLASS_KEY && !(e.ie_Flags & IEF_UP) && e.ie_Char && e.ie_Char < 128)
            buf[got++] = (char)e.ie_Char;
    }
    return got;
}

static void prompt(void)
{
    Printf(&con, "%u.%s> ", number, cwd);
}

/* Lines typed at the console, run as they're ended. */
static int interactive(void)
{
    char line[LINE];
    unsigned n = 0;

    say("Planar Shell. Type Help for help.\n");
    keys_take();
    prompt();
    for (;;) {
        char got[64];
        int64_t r = ReadConsole(got, sizeof got);

        if (r < 0) {
            say("Shell: the console isn't mine\n");
            return 1;
        }
        if (r == 0)
            r = keys_read(got, sizeof got);
        if (r == 0) {
            nap(10);
            continue;
        }
        for (int64_t i = 0; i < r; i++) {
            char ch = got[i];

            if (ch == '\r' || ch == '\n') {
                say("\n");
                line[n] = '\0';
                if (run(line) < 0)
                    return 0;
                n = 0;
                prompt();
            } else if ((ch == 8 || ch == 127) && n) {
                n--;
                say("\b");
            } else if (ch >= 32 && ch < 127 && n < LINE - 1) {
                line[n++] = ch;
                WriteCon(&con, &ch, 1);
            }
        }
    }
}

/* A script's lines, each echoed after the prompt, as if typed. */
static int script(const char *file)
{
    static char text[16384];
    char path[DOS_PATH];
    struct DosEntry e;
    int64_t f, len;
    int failed = 0;

    SNPrintf(path, sizeof path, "%s", cwd);
    if (!AddPart(path, file, sizeof path) || (f = OpenFile(&dos, path, 0, &e)) < 0 || e.de_Dir) {
        Printf(&con, "Shell: no script %s\n", path);
        return 1;
    }
    len = ReadFile(&dos, (uint32_t)f, 0, text, sizeof text - 1);
    CloseFile(&dos, (uint32_t)f);
    if (len < 0)
        return 1;
    text[len] = '\0';
    for (char *line = text; *line;) {
        char *end = line;
        int r;

        while (*end && *end != '\n')
            end++;
        if (*end)
            *end++ = '\0';
        prompt();
        say(line);
        say("\n");
        if ((r = run(line)) < 0)
            break;
        failed += r;
        line = end;
    }
    return failed;
}

int main(uint64_t arg)
{
    struct RDArgs ra = { 0 };
    int64_t a[1] = { 0 };
    char from[DOS_PATH];
    int64_t e;

    /* From a Shell, its directory; from hwdb, no line but the arguments. */
    SplitArgs(arg, from, sizeof from, &ra.rda_Source);
    SNPrintf(cwd, sizeof cwd, "%s", from);
    OpenCon(&con, SIG_CON);
    if (ReadArgs("FROM", a, &ra) != 0) {
        Printf(&con, "Shell: %s%s\n", ra.rda_Why, ra.rda_Why[0] ? ". Template: FROM" : "FROM");
        CloseCon(&con);
        return 1;
    }
    for (uint64_t since = CurrentTime(); (e = OpenDos(&dos, SIG_DOS)) == ERR_NOTFOUND &&
                                         CurrentTime() - since < 5000 * MS;)
        nap(5);
    if (e) {
        say("Shell: no dos\n");
        FreeArgs(&ra);
        CloseCon(&con);
        return 1;
    }
    e = a[0] ? script((const char *)a[0]) : interactive();
    FreeArgs(&ra);
    CloseCon(&con);
    return (int)e;
}
