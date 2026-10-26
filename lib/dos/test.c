/*
 * lib/dos/test.c -- ReadArgs, the path calls and the console on the host
 *
 * lib/dos/args.c, path.c and con.c with the sanitizers (the Makefile's
 * dos-lib-test), against conformance/argscheck.h's cases, which
 * conformance/dosargs.c runs in the guest. AllocVec is the host's
 * malloc here, so a word copied past ReadArgs's block is the address
 * sanitizer's to see. Then random lines of quotes, stars, equals signs
 * and keywords against several templates: each must parse or fail with
 * a reason, and every string given must lie in the block. Then Printf
 * and the console over a fake "con.0" that records each message, one
 * that goes away, and none: the kernel's log.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <planar/drv.h>
#include <planar/exec.h>
#include <planar/utility.h>

#include "argscheck.h"

static uint64_t live;

void *AllocVec(uint64_t byteSize, uint32_t requirements)
{
    uint64_t *p = malloc(byteSize + 16);

    (void)requirements;
    if (!p)
        return nullptr;
    p[0] = byteSize;
    live++;
    return p + 2;
}

void FreeVec(void *memoryBlock)
{
    if (memoryBlock) {
        free((uint64_t *)memoryBlock - 2);
        live--;
    }
}

static uint64_t seed = 1;

static unsigned rnd(unsigned n)
{
    seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
    return (unsigned)(seed >> 33) % n;
}

static bool inside(const void *p, const struct RDArgs *ra)
{
    const uint8_t *b = ra->rda_Buffer;

    return (const uint8_t *)p >= b && (const uint8_t *)p < b + ((const uint64_t *)b)[-2];
}

/* A string given must end inside the block. */
static bool string_inside(const char *s, const struct RDArgs *ra)
{
    while (inside(s, ra) && *s)
        s++;
    return inside(s, ra);
}

static unsigned fuzz(unsigned *cases)
{
    /* Each with its numbers' items, whose values aren't strings. */
    static const struct {
        const char *t;
        unsigned numbers;
    } templates[] = {
        { "FILE/A,ALL/S,TO/K", 0 },    { "FROM/M/A,TO/A,QUIET/S", 0 }, { "TEXT/F", 0 },
        { "N/M/N,X=Y/K", 0 },          { "CMD/A,ARGS/F,Q/S", 0 },      { "A,B/K/N,C/K/F", 1u << 1 },
        { "NAME/A,FILES/M,LINES/K/N", 1u << 2 },
    };
    static const char *const bits[] = {
        " ", "  ", "\"", "*", "*\"", "*n", "=", "a", "bc", "FILE", "to", "ALL", "from",
        "TO=", "x=", "1", "-3", "99999999999999999999", "?", "quiet", "n", "Y", "\t", "args",
        "c=", "lines", "B",
    };
    char line[200];
    int64_t a[8];
    struct RDArgs ra = { 0 };

    for (unsigned round = 0; round < 200000; round++) {
        unsigned k = rnd(sizeof templates / sizeof templates[0]);
        const char *t = templates[k].t;
        unsigned n = 0, parts = rnd(12);
        int64_t r;

        for (unsigned i = 0; i < parts; i++) {
            const char *b = bits[rnd(sizeof bits / sizeof bits[0])];

            while (*b && n < sizeof line - 1)
                line[n++] = *b++;
        }
        line[n] = '\0';
        r = ac_read(t, line, a, 8, 0, &ra);
        if (r != 0) {
            if (r != ERR_BADARG || ra.rda_Buffer)
                return __LINE__;
            continue;
        }
        /* Every value given but a switch's 1 is in the block: a number's
         * whole, a string to its NUL. (A /M's array of N/M/N's numbers is
         * pointers, which hold a 0 byte.) */
        for (unsigned i = 0; i < 8; i++) {
            const char *v = (const char *)a[i];

            if (!a[i] || a[i] == 1)
                continue;
            if (templates[k].numbers & 1u << i ? !inside(v, &ra) || !inside(v + 7, &ra)
                                               : !string_inside(v, &ra))
                return __LINE__;
        }
        (*cases)++;
    }
    FreeArgs(&ra);
    return 0;
}

/* --- A console to write to. --- */

static struct {
    bool up;                        /* ConnectPort finds the console */
    const char *name;               /* its name: "con.0" if nullptr */
    const char *typed;              /* what a DRV_READ answers with (DoMsg) */
    int32_t read_error;
    bool slow;                      /* DoMsgUntil's deadline passes instead */
    int fail_after;                 /* DoMsg's that work before it goes; -1: never */
    unsigned reports;               /* ReportUnresponsive's */
    unsigned msgs, connects;
    char got[1024];                 /* what the messages carried */
    unsigned n;
    char log[8][200];               /* KLog's lines */
    unsigned logs;
    struct DrvRequest req;
} fake;

uint64_t CurrentTime(void)
{
    return 1;
}

int64_t ReportUnresponsive(const char *portName)
{
    if (strcmp(portName, fake.name ? fake.name : "con.0") != 0)
        abort();
    fake.reports++;
    return 0;
}

int64_t ConnectPort(struct MsgPort *mp, const char *name, int64_t sigbit, uint32_t slots,
                    uint64_t heap)
{
    (void)mp, (void)sigbit, (void)slots, (void)heap;
    if (strcmp(name, fake.name ? fake.name : "con.0") != 0)
        abort();
    fake.connects++;
    return fake.up ? 0 : ERR_NOTFOUND;
}

void ClosePort(struct MsgPort *mp)
{
    (void)mp;
}

void *AllocMsg(struct MsgPort *mp, uint64_t size)
{
    (void)mp;
    return size <= sizeof fake.req ? &fake.req : nullptr;
}

int64_t DoMsgUntil(struct MsgPort *mp, struct Message *m, uint64_t deadline)
{
    struct DrvRequest *r = (struct DrvRequest *)m;

    (void)mp;
    if (deadline <= 1)
        abort();                    /* a deadline in the future, always */
    if (fake.slow)
        return ERR_TIMEOUT;
    if (fake.fail_after == 0)
        return ERR_PORT_DIED;
    if (fake.fail_after > 0)
        fake.fail_after--;
    if (r->dr_Op != DRV_WRITE || r->dr_Len == 0 || r->dr_Len > sizeof r->dr_Data ||
        fake.n + r->dr_Len > sizeof fake.got)
        abort();
    memcpy(fake.got + fake.n, r->dr_Data, r->dr_Len);
    fake.n += r->dr_Len;
    fake.msgs++;
    return 0;
}

/* ReadCon's: a console window answering a read with what's typed. */
int64_t DoMsg(struct MsgPort *mp, struct Message *m)
{
    struct DrvRequest *r = (struct DrvRequest *)m;
    uint32_t n = fake.typed ? (uint32_t)strlen(fake.typed) : 0;

    (void)mp;
    if (r->dr_Op != DRV_READ || r->dr_Len > sizeof r->dr_Data)
        abort();
    m->mn_Error = fake.read_error;
    n = n < r->dr_Len ? n : r->dr_Len;
    memcpy(r->dr_Data, fake.typed, n);
    r->dr_Len = n;
    if (fake.typed)
        fake.typed += n;
    return 0;
}

int64_t KLog(const char *text)
{
    if (fake.logs < 8)
        snprintf(fake.log[fake.logs++], sizeof fake.log[0], "%s", text);
    return 0;
}

static void fake_reset(bool up, int fail_after)
{
    memset(&fake, 0, sizeof fake);
    fake.up = up;
    fake.fail_after = fail_after;
}

static bool got(const char *want)
{
    return fake.n == strlen(want) && memcmp(fake.got, want, fake.n) == 0;
}

#define CC(cond) \
    if (!(cond)) \
        return __LINE__; \
    (*cases)++

static unsigned console(unsigned *cases)
{
    struct Con c;
    char big[300];

    fake_reset(true, -1);
    OpenCon(&c, 16);
    CC(c.cn_Open && Printf(&c, "%s=%d, %lu %S%%\n", "x", -5, 123456789012ULL, STR("str")) == 24);
    CC(got("x=-5, 123456789012 str%\n") && fake.msgs == 1);
    for (unsigned i = 0; i < sizeof big - 1; i++)
        big[i] = (char)('a' + i % 26);
    big[sizeof big - 1] = '\0';
    fake.n = fake.msgs = 0;
    CC(Printf(&c, "%s", big) == 299 && got(big) && fake.msgs == 5);   /* 64 a message */
    fake.n = fake.msgs = 0;
    big[64] = '\0';
    PutStr(&c, big);
    CC(got(big) && fake.msgs == 1);
    big[64] = 'm';
    big[65] = '\0';
    fake.n = fake.msgs = 0;
    WriteCon(&c, big, 65);
    CC(got(big) && fake.msgs == 2);
    fake.n = fake.msgs = 0;
    CC(Printf(&c, "") == 0 && fake.msgs == 0);
    CloseCon(&c);
    CC(fake.logs == 0 && !c.cn_Open);

    /* The console goes, and is found again once. */
    fake_reset(true, 1);
    OpenCon(&c, 16);
    PutStr(&c, "one\n");
    PutStr(&c, "two\n");                /* fails, reconnects, then goes */
    CC(fake.connects == 2 && got("one\n") && c.cn_Retried && !c.cn_Open);
    CC(fake.logs == 1 && strcmp(fake.log[0], "two") == 0);
    PutStr(&c, "three");
    CloseCon(&c);
    CC(fake.connects == 2 && fake.logs == 2 && strcmp(fake.log[1], "three") == 0);

    /* A console that misses its deadline: reported, and the log after. */
    fake_reset(true, -1);
    fake.slow = true;
    OpenCon(&c, 16);
    PutStr(&c, "late\n");
    CC(fake.reports == 2 && fake.connects == 2 && !c.cn_Open);     /* once more, then the log */
    CC(fake.logs == 1 && strcmp(fake.log[0], "late") == 0);
    CloseCon(&c);

    /* None: the log, a line at a time, and a long one cut. */
    fake_reset(false, -1);
    OpenCon(&c, 16);
    CC(!c.cn_Open);
    Printf(&c, "a\nbc %d\n%s", 7, "rest");
    CC(fake.logs == 2 && strcmp(fake.log[0], "a") == 0 && strcmp(fake.log[1], "bc 7") == 0);
    CloseCon(&c);
    CC(fake.logs == 3 && strcmp(fake.log[2], "rest") == 0);
    fake_reset(false, -1);
    OpenCon(&c, 16);
    memset(big, 'z', 200);
    big[200] = '\0';
    PutStr(&c, big);
    CloseCon(&c);
    CC(fake.logs == 2 && strlen(fake.log[0]) == sizeof c.cn_Line - 1 &&
       strlen(fake.log[1]) == 200 - (sizeof c.cn_Line - 1));

    /* A console window's (V8 step 5): named by the Shell's line, read a line at a time. */
    {
        char cwd[DOS_PATH], buf[64];
        const char *args;

        SplitArgs((uint64_t)"SYS:C\nFROM x", cwd, sizeof cwd, &args);
        CC(!strcmp(cwd, "SYS:C") && !strcmp(args, "FROM x") && !strcmp(ConName(), "con.0"));
        SplitArgs((uint64_t)"SYS:EFI\tcon.1\nFROM x", cwd, sizeof cwd, &args);
        CC(!strcmp(cwd, "SYS:EFI") && !strcmp(args, "FROM x") && !strcmp(ConName(), "con.1"));
        fake_reset(true, -1);
        fake.name = "con.1";
        fake.typed = "a line\nand more";
        OpenCon(&c, 16);
        PutStr(&c, "> ");
        CC(c.cn_Open && got("> "));
        CC(ReadCon(&c, buf, 4) == 4 && !memcmp(buf, "a li", 4));
        CC(ReadCon(&c, buf, sizeof buf) == 11 && !memcmp(buf, "ne\nand more", 11));
        CC(ReadCon(&c, buf, sizeof buf) == 0);                 /* the end of the input */
        fake.read_error = ERR_BADARG;
        CC(ReadCon(&c, buf, sizeof buf) == ERR_BADARG);
        CloseCon(&c);
        CC(ReadCon(&c, buf, sizeof buf) == ERR_PORT_DIED);     /* closed */
        SetConName("a name far longer than a console's");
        CC(strlen(ConName()) == CON_NAME - 1);
        SetConName(nullptr);
        CC(!strcmp(ConName(), "con.0"));
    }
    return 0;
}

int main(void)
{
    unsigned cases = 0, fuzzed = 0, line;

    if ((line = args_check(&cases))) {
        printf("dos-lib-test: FAILED: argscheck.h:%u\n", line);
        return 1;
    }
    if ((line = fuzz(&fuzzed))) {
        printf("dos-lib-test: FAILED: test.c:%u\n", line);
        return 1;
    }
    if ((line = console(&cases))) {
        printf("dos-lib-test: FAILED: test.c:%u\n", line);
        return 1;
    }
    if (live) {
        printf("dos-lib-test: FAILED: %llu blocks not freed\n", (unsigned long long)live);
        return 1;
    }
    printf("dos-lib-test: ok: %u cases, %u random lines parsed\n", cases, fuzzed);
    return 0;
}
