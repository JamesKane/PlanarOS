/*
 * cmd/shell.c -- the Shell (v4-plan step 7): commands from C:, run as
 * processes, their output on the console
 *
 *   Shell              read lines from the serial console (decision 1)
 *   Shell -s FILE      run FILE's lines, then end: 0 if all succeeded
 *
 * A line's first word is a built-in (CD, Help, EndShell) or a command,
 * found in C: unless it names its volume or directory; the rest of the
 * line is its arguments, and it's given the current directory with them
 * (dos_args). The Shell waits for it and says how it ended if not well.
 * Input is the console's (GrantConsole, by hwdb), polled every 10 ms,
 * echoed, with backspace.
 */
#include <planar/dos.h>
#include <planar/lease.h>
#include <planar/syscalls.h>

constexpr int64_t SIG_CON = 16;
constexpr int64_t SIG_DOS = 17;
constexpr int64_t SIG_CHILD = 18;
constexpr int64_t SIG_TICK = 19;
constexpr uint64_t MS = 1000000;
constexpr unsigned LINE = 200;

static struct con con;
static struct dos dos;
static char cwd[DOS_PATH] = "SYS:";
static unsigned number = 1;             /* the Amiga's CLI number, for the prompt */

static void say(const char *s)
{
    con_puts(&con, s);
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

/* CD DIR: a directory that exists becomes the current one. */
static int cd(const char *arg)
{
    char path[DOS_PATH];
    struct DosEntry e;

    if (!*arg) {
        say(cwd);
        say("\n");
        return 0;
    }
    if (arg[0] == '/' && !arg[1]) {     /* the parent */
        unsigned n = 0, cut = 0;

        while (cwd[n]) {
            if (cwd[n] == '/' || cwd[n] == ':')
                cut = cwd[n] == ':' ? n + 1 : n;
            n++;
        }
        cwd[cut] = '\0';
        return 0;
    }
    dos_path(cwd, arg, path, sizeof path);
    if (dos_stat(&dos, path, &e) != 0 || !e.de_Dir) {
        say("CD: not a directory: ");
        say(path);
        say("\n");
        return 1;
    }
    for (unsigned i = 0; i < DOS_PATH; i++)
        if (!(cwd[i] = path[i]))
            break;
    return 0;
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
    dos_path(place ? cwd : "C:", name, path, sizeof path);
    for (const char *s = cwd; *s && k < DOS_ARGS - 2; s++)
        args[k++] = *s;
    args[k++] = '\n';
    for (const char *s = rest; *s && k < DOS_ARGS - 1; s++)
        args[k++] = *s;
    args[k] = '\0';
    if ((p = dos_run(&dos, path, args, SIG_CHILD, 0)) < 0) {
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
    say(name);
    if (pi.pi_State == PROCS_EXITED) {
        say(" failed returncode ");
        con_dec(&con, (uint64_t)(pi.pi_ExitCode < 0 ? -pi.pi_ExitCode : pi.pi_ExitCode));
    } else {
        say(" faulted");
    }
    say("\n");
    return 1;
}

static void prompt(void)
{
    con_dec(&con, number);
    say(".");
    say(cwd);
    say("> ");
}

/* Lines typed at the console, run as they're ended. */
static int interactive(void)
{
    char line[LINE];
    unsigned n = 0;

    say("Planar Shell. Type Help for help.\n");
    prompt();
    for (;;) {
        char got[64];
        int64_t r = ReadConsole(got, sizeof got);

        if (r < 0) {
            say("Shell: the console isn't mine\n");
            return 1;
        }
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
                con_write(&con, &ch, 1);
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

    dos_path(cwd, file, path, sizeof path);
    if ((f = dos_file(&dos, path, 0, &e)) < 0 || e.de_Dir) {
        say("Shell: no script ");
        say(path);
        say("\n");
        return 1;
    }
    len = dos_read(&dos, (uint32_t)f, 0, text, sizeof text - 1);
    dos_close_file(&dos, (uint32_t)f);
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
    const char *args = arg ? (const char *)arg : "";
    int64_t e;

    con_open(&con, SIG_CON);
    for (uint64_t since = CurrentTime(); (e = dos_open(&dos, SIG_DOS)) == ERR_NOTFOUND &&
                                         CurrentTime() - since < 5000 * MS;)
        nap(5);
    if (e) {
        say("Shell: no dos\n");
        con_close(&con);
        return 1;
    }
    /* Started by hwdb, not from a Shell: its line is just the arguments. */
    while (*args == ' ')
        args++;
    if (args[0] == '-' && args[1] == 's' && args[2] == ' ') {
        int failed = script(args + 3);

        con_close(&con);
        return failed;
    }
    e = interactive();
    con_close(&con);
    return (int)e;
}
