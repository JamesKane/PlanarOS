/*
 * cmd/shell.c -- the Shell (v4-plan step 7): commands from C:, run as
 * processes, their output on the console
 *
 *   Shell              read lines from the serial console (decision 1)
 *   Shell FROM FILE    run FILE's lines, then end: 0 if all succeeded
 *   Shell SERIAL       the serial console's lines alone: intuition has
 *                      the keyboard (hwdb's, with the desktop: V8 step 4)
 *   Shell CON con.1    in a console window (V8 step 5): its lines, and
 *                      every command's output, there
 *
 * Its template is "FROM,SERIAL/S,CON/K" (ReadArgs). Started from a Shell, it begins in
 * that one's current directory, on its console. A console window's Shell
 * is told its console by its line, as its commands are (SplitArgs); CON
 * says it outright. There the window edits and echoes each line, and
 * the Shell reads them whole (ReadCon); the end of the input (Ctrl-\)
 * or the window's going ends it. Its number, in the prompt, is 1 on
 * con.0 and n + 1 on con.n, and the prompt's in the window's green. A line's first word is a built-in (CD,
 * Help, EndShell) or a command, found in C: unless it names its volume
 * or directory; the rest of the line is its arguments, and it's given
 * the current directory with them (SplitArgs). The Shell waits for it
 * and says how it ended if not well.
 * Input is the console's (GrantConsole, by hwdb) and a USB keyboard's,
 * from input's ring (planar/devices/input.h), which the Shell takes when
 * input is there and again after it restarts, unless SERIAL. Both are polled every
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
static const char TEMPLATE[] = "FROM,SERIAL/S,CON/K";

static struct Con con;
static struct Dos dos;
static char cwd[DOS_PATH] = "SYS:";
static bool serial_only;                /* SERIAL: no keyboard of input's */
static bool windowed;                   /* on a console window's console: its lines */
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
    if (windowed) {                     /* its console, this one's */
        args[k++] = '\t';
        for (const char *s = ConName(); *s && k < DOS_ARGS - 2; s++)
            args[k++] = *s;
    }
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
    if (windowed)
        Printf(&con, "\x1b[32m%u.%s>\x1b[0m ", number, cwd);  /* text.shell.prompt */
    else
        Printf(&con, "%u.%s> ", number, cwd);
}

/* Lines from a console window, which has edited and echoed them. */
static int window_lines(void)
{
    char line[LINE];
    unsigned n = 0;

    say("Planar Shell. Type Help for help.\n");
    prompt();
    for (;;) {
        char got[64];
        int64_t r = ReadCon(&con, got, sizeof got);

        if (r < 0)
            return 1;                   /* the window went: no one's typing */
        if (r == 0)
            return 0;                   /* the end of the input */
        for (int64_t i = 0; i < r; i++) {
            if (got[i] != '\n') {
                if (n < LINE - 1)
                    line[n++] = got[i];
                continue;
            }
            line[n] = '\0';
            n = 0;
            if (run(line) < 0)
                return 0;
            prompt();
        }
    }
}

/* Lines typed at the console, run as they're ended. */
static int interactive(void)
{
    char line[LINE];
    unsigned n = 0;

    say("Planar Shell. Type Help for help.\n");
    if (!serial_only)
        keys_take();
    prompt();
    for (;;) {
        char got[64];
        int64_t r = ReadConsole(got, sizeof got);

        if (r < 0) {
            say("Shell: the console isn't mine\n");
            return 1;
        }
        if (r == 0 && !serial_only)
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
    int64_t a[3] = { 0 };
    char from[DOS_PATH];
    int64_t e;

    /* From a Shell, its directory; from hwdb, no line but the arguments. */
    SplitArgs(arg, from, sizeof from, &ra.rda_Source);
    SNPrintf(cwd, sizeof cwd, "%s", from);
    OpenCon(&con, SIG_CON);
    if (ReadArgs(TEMPLATE, a, &ra) != 0) {
        Printf(&con, "Shell: %s%s%s\n", ra.rda_Why, ra.rda_Why[0] ? ". Template: " : "", TEMPLATE);
        CloseCon(&con);
        return 1;
    }
    if ((e = OpenDosUntil(&dos, SIG_DOS, CurrentTime() + 5000 * MS)) != 0) {
        say("Shell: no dos\n");
        FreeArgs(&ra);
        CloseCon(&con);
        return 1;
    }
    serial_only = a[1] != 0;
    if (a[2]) {                         /* CON: that console, not the line's */
        CloseCon(&con);
        SetConName((const char *)a[2]);
        OpenCon(&con, SIG_CON);
    }
    windowed = Stricmp(ConName(), "con.0") != 0;
    if (windowed) {
        const char *d = ConName() + 4;     /* "con.n", n 1 to 8 */

        number = *d >= '1' && *d <= '9' ? 1u + (unsigned)(*d - '0') : 1;
        if (!con.cn_Open) {
            Printf(&con, "Shell: no console %s\n", ConName());
            FreeArgs(&ra);
            CloseCon(&con);
            return 1;
        }
    }
    e = a[0] ? script((const char *)a[0]) : windowed ? window_lines() : interactive();
    FreeArgs(&ra);
    CloseCon(&con);
    return (int)e;
}
