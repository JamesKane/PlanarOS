/*
 * conformance/argscheck.h -- ReadArgs and V36's path calls as the RKM
 * says (V6 step 6)
 *
 * Templates with every modifier here: positional and keyword values,
 * '=' and quotes, aliases, numbers at their limits, /M giving back to
 * the /A items after it, /F's rest of the line, and each error with its
 * reason. Then AddPart's volumes, steps up and room, FilePart and
 * PathPart. Shared by conformance/dosargs.c, in the guest, and
 * lib/dos/test.c, on the host with the sanitizers.
 */
#ifndef CONFORMANCE_ARGSCHECK_H
#define CONFORMANCE_ARGSCHECK_H

#include <stdint.h>

#include <planar/dos.h>
#include <planar/syscalls.h>

#define AC(cond) \
    if (!(cond)) \
        return __LINE__; \
    (*cases)++

static bool ac_same(const char *a, const char *b)
{
    if (!a || !b)
        return a == b;
    while (*a && *a == *b)
        a++, b++;
    return *a == *b;
}

/* ReadArgs of 'line', its array cleared to 'fill' first. */
static int64_t ac_read(const char *argTemplate, const char *line, int64_t *a, unsigned n,
                       int64_t fill, struct RDArgs *ra)
{
    for (unsigned i = 0; i < n; i++)
        a[i] = fill;
    FreeArgs(ra);
    ra->rda_Source = line;
    return ReadArgs(argTemplate, a, ra);
}

#define READ(t, line) ac_read((t), (line), a, 8, 0, &ra)
#define S(i)  ((const char *)a[i])
#define N(i)  (*(const int64_t *)a[i])
#define M(i)  ((const char *const *)a[i])

static unsigned ac_args(unsigned *cases)
{
    struct RDArgs ra = { 0 };
    int64_t a[8];
    const char *const *m;

    /* The plan's template. */
    AC(READ("FILE/A,ALL/S,TO/K", "foo") == 0 && ac_same(S(0), "foo") && !a[1] && !a[2]);
    AC(READ("FILE/A,ALL/S,TO/K", "foo ALL TO bar") == 0 && ac_same(S(0), "foo") && a[1] == 1 &&
       ac_same(S(2), "bar"));
    AC(READ("FILE/A,ALL/S,TO/K", "  to=bar\tfoo all ") == 0 && ac_same(S(0), "foo") &&
       a[1] == 1 && ac_same(S(2), "bar"));
    AC(READ("FILE/A,ALL/S,TO/K", "") == ERR_BADARG && ac_same(ra.rda_Why, "FILE is required"));
    AC(READ("FILE/A,ALL/S,TO/K", "foo bar") == ERR_BADARG &&
       ac_same(ra.rda_Why, "too many arguments"));
    AC(READ("FILE/A,ALL/S,TO/K", "foo TO") == ERR_BADARG &&
       ac_same(ra.rda_Why, "TO needs a value"));
    AC(READ("FILE/A,ALL/S,TO/K", "foo ALL=1") == ERR_BADARG &&
       ac_same(ra.rda_Why, "ALL takes no value"));
    AC(READ("FILE/A,ALL/S,TO/K", "foo all ALL") == ERR_BADARG &&
       ac_same(ra.rda_Why, "ALL is given twice"));
    AC(READ("FILE/A,ALL/S,TO/K", "foo TO a TO b") == ERR_BADARG &&
       ac_same(ra.rda_Why, "TO is given twice"));
    AC(READ("FILE/A,ALL/S,TO/K", "f.txt x=y") == ERR_BADARG);
    AC(READ("FILE/A", "x=y") == 0 && ac_same(S(0), "x=y"));    /* not a keyword: a word */
    AC(READ("FILE/A,ALL/S,TO/K", "?") == ERR_BADARG && ra.rda_Why[0] == '\0');
    AC(READ("FILE/A,ALL/S,TO/K", " ? ") == ERR_BADARG && ra.rda_Why[0] == '\0');
    AC(READ("FILE,X/S", "? x") == 0 && ac_same(S(0), "?") && a[1] == 1);
    ra.rda_Source = nullptr;
    FreeArgs(&ra);
    AC(ReadArgs("FILE/A", a, &ra) == ERR_BADARG && ac_same(ra.rda_Why, "FILE is required"));
    AC(ReadArgs("FILE", a, nullptr) == ERR_BADARG);

    /* Defaults: what isn't given is left. */
    AC(ac_read("FILE/A,LINES/K/N,Q/S", "f", a, 8, 77, &ra) == 0 && ac_same(S(0), "f") &&
       a[1] == 77 && a[2] == 77);

    /* Quotes: never keywords; spaces and escapes. */
    AC(READ("FILE/A,ALL/S", "\"all\"") == 0 && ac_same(S(0), "all") && !a[1]);
    AC(READ("FILE/A", "\"a b  c\"") == 0 && ac_same(S(0), "a b  c"));
    AC(READ("FILE/A", "\"x*\"y*n**z*e\"") == 0 && ac_same(S(0), "x\"y\n*z\x1b"));
    AC(READ("FILE/A", "\"\"") == 0 && ac_same(S(0), ""));
    AC(READ("FILE/A", "\"abc") == ERR_BADARG && ac_same(ra.rda_Why, "a quote isn't closed"));
    AC(READ("FILE/A", "\"abc*\"") == ERR_BADARG);
    AC(READ("FILE/A,TO/K", "TO=\"a b\" f") == 0 && ac_same(S(1), "a b") && ac_same(S(0), "f"));
    AC(READ("FILE/A,TO/K", "TO=\"a b") == ERR_BADARG);
    AC(READ("FILE/A,TO/K", "TO=\"\" f") == 0 && ac_same(S(1), "") && ac_same(S(0), "f"));
    AC(READ("FILE/A,TO/K", "TO= x f") == 0 && ac_same(S(1), "x") && ac_same(S(0), "f"));
    AC(READ("FILE/A,TO/K", "TO=a=b f") == 0 && ac_same(S(1), "a=b"));
    AC(READ("A,B", "\"one\"\"two\"") == 0 && ac_same(S(0), "one") && ac_same(S(1), "two"));

    /* Aliases: any name, and the first in the reason. */
    AC(READ("FILE/A,TO=AS/K", "f as x") == 0 && ac_same(S(1), "x"));
    AC(READ("FILE/A,TO=AS/K", "f TO=x") == 0 && ac_same(S(1), "x"));
    AC(READ("FILE/A,TO=AS/K", "f as") == ERR_BADARG && ac_same(ra.rda_Why, "TO needs a value"));
    AC(READ("NAME=FILE/A", "file=x") == 0 && ac_same(S(0), "x"));

    /* Numbers. */
    AC(READ("LINES/K/N", "LINES 42") == 0 && N(0) == 42);
    AC(READ("LINES/K/N", "lines=-7") == 0 && N(0) == -7);
    AC(READ("LINES/N", "9223372036854775807") == 0 && N(0) == INT64_MAX);
    AC(READ("LINES/N", "-9223372036854775808") == 0 && N(0) == INT64_MIN);
    AC(READ("LINES/N", "9223372036854775808") == ERR_BADARG &&
       ac_same(ra.rda_Why, "LINES needs a number"));
    AC(READ("LINES/N", "-9223372036854775809") == ERR_BADARG);
    AC(READ("LINES/N", "12x") == ERR_BADARG && READ("LINES/N", "-") == ERR_BADARG);
    AC(READ("LINES/N", "0x10") == ERR_BADARG);                 /* decimal, as AmigaDOS's */
    AC(READ("A/N,B/N", "1 2") == 0 && N(0) == 1 && N(1) == 2);

    /* /M, and its words given back to the /A items after it. */
    AC(READ("FROM/M/A,TO/A", "a b c d") == 0 && (m = M(0)) && ac_same(m[0], "a") &&
       ac_same(m[1], "b") && ac_same(m[2], "c") && !m[3] && ac_same(S(1), "d"));
    AC(READ("FROM/M/A,TO/A", "a b") == 0 && ac_same(M(0)[0], "a") && !M(0)[1] &&
       ac_same(S(1), "b"));
    AC(READ("FROM/M/A,TO/A", "a") == ERR_BADARG && ac_same(ra.rda_Why, "FROM is required"));
    AC(READ("FROM/M/A,TO/A", "") == ERR_BADARG);
    AC(READ("FROM/M/A,TO/A", "a b TO c") == 0 && !M(0)[2] && ac_same(M(0)[1], "b") &&
       ac_same(S(1), "c"));
    AC(READ("FROM/M,TO/A,X/A", "a b c") == 0 && ac_same(M(0)[0], "a") && !M(0)[1] &&
       ac_same(S(1), "b") && ac_same(S(2), "c"));
    AC(READ("FROM/M,TO/A,X/A", "a b") == 0 && !a[0] && ac_same(S(1), "a") && ac_same(S(2), "b"));
    AC(READ("FROM/M,TO/A,X/A", "a") == ERR_BADARG && ac_same(ra.rda_Why, "X is required"));
    AC(READ("NAME/A,FILES/M", "n") == 0 && ac_same(S(0), "n") && !a[1]);
    AC(READ("NAME/A,FILES/M", "n x \"y z\"") == 0 && ac_same(M(1)[0], "x") &&
       ac_same(M(1)[1], "y z") && !M(1)[2]);
    AC(READ("FILES/M,TO/K", "a TO x b") == 0 && ac_same(M(0)[0], "a") && ac_same(M(0)[1], "b") &&
       !M(0)[2] && ac_same(S(1), "x"));
    AC(READ("FILES/M,TO/K", "files=a b") == 0 && ac_same(M(0)[0], "a") && ac_same(M(0)[1], "b"));
    AC(READ("N/M/N", "1 -2 3") == 0 && *(const int64_t *)M(0)[0] == 1 &&
       *(const int64_t *)M(0)[1] == -2 && *(const int64_t *)M(0)[2] == 3 && !M(0)[3]);
    AC(READ("N/M/N", "1 x") == ERR_BADARG);

    /* /F: the rest, as it stands. */
    AC(READ("TEXT/F", "  hello \"there\"   world  ") == 0 &&
       ac_same(S(0), "hello \"there\"   world"));
    AC(READ("CMD/A,ARGS/F", "run a b=c TO") == 0 && ac_same(S(0), "run") &&
       ac_same(S(1), "a b=c TO"));
    AC(READ("CMD/A,ARGS/F", "run") == 0 && !a[1]);
    AC(READ("CMD/A,ARGS/F", "\"r u\" x") == 0 && ac_same(S(0), "r u") && ac_same(S(1), "x"));
    AC(READ("X/K,REST/K/F", "REST=a  b c ") == 0 && ac_same(S(1), "a  b c") && !a[0]);
    AC(READ("X/K,REST/K/F", "x 1 REST  a X b") == 0 && ac_same(S(0), "1") &&
       ac_same(S(1), "a X b"));
    AC(READ("X/K,REST/K/F", "REST") == 0 && ac_same(S(1), ""));
    {
        static char word[600];

        for (unsigned i = 0; i < sizeof word - 1; i++)
            word[i] = (char)('a' + i % 26);
        AC(READ("TEXT/F", word) == 0 && ac_same(S(0), word));     /* copied twice */
        word[0] = '"';
        AC(READ("TEXT/F,Q/S", word) == ERR_BADARG);                /* a quote not closed */
    }

    /* The template's own errors. */
    AC(READ("A/X", "a") == ERR_BADARG && ac_same(ra.rda_Why, "the command's template is wrong"));
    AC(READ("A/T", "a") == ERR_BADARG);                        /* no /T */
    AC(READ("A/M,B/M", "a") == ERR_BADARG);
    AC(READ("A/S/N", "a") == ERR_BADARG);
    AC(READ("", "a") == ERR_BADARG && READ("A,,B", "a") == ERR_BADARG);
    AC(READ("A,B,C,D,E,F,G,H,I,J,K,L,M,N,O,P,Q,R,S,T,U,V,W,X,Y,Z,AA,AB,AC,AD,AE,AF",
            "\"x\"") == 0 && ac_same(S(0), "x"));               /* 32 items */
    AC(READ("A,B,C,D,E,F,G,H,I,J,K,L,M,N,O,P,Q,R,S,T,U,V,W,X,Y,Z,AA,AB,AC,AD,AE,AF,AG",
            "\"x\"") == ERR_BADARG);
    AC(READ(" FILE/a , all/s ", "x ALL") == 0 && ac_same(S(0), "x") && a[1] == 1);

    /* A failure leaves nothing to free; FreeArgs twice is harmless. */
    AC(READ("FILE/A", "") == ERR_BADARG && !ra.rda_Buffer);
    AC(READ("FILE/A", "x") == 0 && ra.rda_Buffer);
    FreeArgs(&ra);
    FreeArgs(&ra);
    AC(!ra.rda_Buffer);
    return 0;
}

static unsigned ac_add(const char *dir, const char *file, unsigned size, const char *want,
                       unsigned *cases)
{
    char buf[64];
    unsigned i = 0;

    for (; dir[i]; i++)
        buf[i] = dir[i];
    buf[i] = '\0';
    AC(AddPart(buf, file, size) == (want != nullptr) && ac_same(buf, want ? want : dir));
    return 0;
}

#define ADD(d, f, s, w) \
    if ((line = ac_add((d), (f), (s), (w), cases))) \
        return line

static unsigned ac_paths(unsigned *cases)
{
    unsigned line;
    const char *p = "SYS:a/b";

    ADD("SYS:a", "b", 64, "SYS:a/b");
    ADD("SYS:", "b", 64, "SYS:b");
    ADD("SYS:a/", "b", 64, "SYS:a/b");
    ADD("SYS:a/b", "/c", 64, "SYS:a/c");
    ADD("SYS:a/b", "//c", 64, "SYS:c");
    ADD("SYS:a/b", "///c", 64, "SYS:c");          /* not past the volume */
    ADD("SYS:a", "/", 64, "SYS:");
    ADD("SYS:a", "C:x", 64, "C:x");
    ADD("SYS:a/b", ":x", 64, "SYS:x");
    ADD("SYS:a/b", ":", 64, "SYS:");
    ADD("a/b", ":x", 64, "x");
    ADD("SYS:a", "", 64, "SYS:a");
    ADD("", "x", 64, "x");
    ADD("a", "/x", 64, "x");
    ADD("SYS:a", "b/c", 64, "SYS:a/b/c");
    ADD("SYS:", "ab", 7, "SYS:ab");
    ADD("SYS:", "ab", 6, nullptr);                 /* no room: as it was */
    ADD("SYS:a", "C:long", 6, nullptr);
    ADD("SYS:a", "C:abc", 6, "C:abc");

    AC(ac_same(FilePart(p), "b") && FilePart(p) == p + 6);
    AC(ac_same(FilePart("SYS:"), "") && ac_same(FilePart("x"), "x") &&
       ac_same(FilePart("SYS:a"), "a"));
    AC(PathPart(p) == p + 5 && ac_same(PathPart(p), "/b"));
    AC(ac_same(PathPart("SYS:a"), "a") && ac_same(PathPart("x"), "x"));
    AC(ac_same(PathPart("a/b:c"), "c") && ac_same(PathPart("a/b/c"), "/c"));
    return 0;
}

/* 0, or the line of the first case that failed. */
static unsigned args_check(unsigned *cases)
{
    unsigned line;

    if ((line = ac_args(cases)) || (line = ac_paths(cases)))
        return line;
    return 0;
}

#undef READ
#undef S
#undef N
#undef M
#undef ADD

#endif /* CONFORMANCE_ARGSCHECK_H */
