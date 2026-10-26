/*
 * lib/dos/args.c -- a command's line: SplitArgs, and ReadArgs and
 * FreeArgs, its arguments by an AmigaDOS template (V36's, planar/dos.h)
 *
 * The template's items first, then the line a word at a time. A word
 * that's one of an item's names is its keyword, and takes the next
 * word, or what follows its '=', as the value. Any other word fills the
 * first positional item not yet given: not /K, not /S. A /M item takes
 * every positional word from there on, and gives back from its end
 * what the /A items after it need. A /F item takes the rest of the line
 * as it stands, from where it's reached.
 *
 * The words are copied, unquoted, into one AllocVec block, after room
 * for the numbers and the /M's array: what 'array' points into.
 */
#include <planar/dos.h>
#include <planar/exec.h>
#include <planar/syscalls.h>
#include <planar/utility.h>

constexpr unsigned ITEMS = 32;

enum : uint8_t {
    I_REQUIRED = 1,                 /* /A */
    I_KEYWORD = 2,                  /* /K */
    I_SWITCH = 4,                   /* /S */
    I_NUMBER = 8,                   /* /N */
    I_MULTI = 16,                   /* /M */
    I_REST = 32,                    /* /F */
};

struct item {
    struct Str names;               /* "TO=AS" */
    uint8_t flags;
    bool given;
};

struct word {
    const char *start;              /* in the line */
    char *text;                     /* the copy, unquoted */
    unsigned eq;                    /* where its first '=' is, unquoted; 0: none */
};

struct parse {
    struct RDArgs *ra;
    struct item item[ITEMS];
    unsigned items;
    const char *p;                  /* the line, as far as it's read */
    char *text;                     /* where the next word is copied */
    int64_t *number;                /* where the next /N's value goes */
    char **multi;                   /* the /M's words */
    unsigned multis;
    int multi_item;                 /* -1: none */
};

static bool space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static unsigned length(const char *s)
{
    unsigned n = 0;

    while (s[n])
        n++;
    return n;
}

/* An item's first name, for rda_Why. */
static struct Str name(const struct item *it)
{
    struct Str head, tail;

    StrSplit(it->names, '=', &head, &tail);
    return head;
}

static int64_t why(struct parse *ps, const char *format, const struct item *it)
{
    SNPrintf(ps->ra->rda_Why, sizeof ps->ra->rda_Why, format, it ? name(it) : STR(""));
    return ERR_BADARG;
}

static bool parse_template(struct parse *ps, const char *argTemplate)
{
    struct Str rest = StrFromC(argTemplate), one;

    ps->multi_item = -1;
    while (rest.st_Len) {
        struct Str mods;
        struct item *it;

        StrSplit(rest, ',', &one, &rest);
        if (ps->items == ITEMS)
            return false;
        it = &ps->item[ps->items];
        StrSplit(StrTrim(one), '/', &it->names, &mods);
        if (!it->names.st_Len)
            return false;
        for (uint64_t i = 0; i < mods.st_Len; i++) {
            switch (ToUpper(mods.st_Data[i])) {
            case 'A': it->flags |= I_REQUIRED; break;
            case 'K': it->flags |= I_KEYWORD; break;
            case 'S': it->flags |= I_SWITCH; break;
            case 'N': it->flags |= I_NUMBER; break;
            case 'M': it->flags |= I_MULTI; break;
            case 'F': it->flags |= I_REST; break;
            case '/': continue;
            default: return false;      /* /T and the rest aren't here */
            }
        }
        if (it->flags & I_MULTI) {
            if (ps->multi_item >= 0 || it->flags & I_REST)
                return false;           /* one /M, as AmigaDOS has */
            ps->multi_item = (int)ps->items;
        }
        if (it->flags & I_SWITCH && it->flags & ~(I_SWITCH | I_KEYWORD))
            return false;
        ps->items++;
    }
    return ps->items > 0;
}

/* The item one of whose names is 'key', or -1. */
static int keyword(const struct parse *ps, struct Str key)
{
    for (unsigned i = 0; i < ps->items; i++) {
        struct Str rest = ps->item[i].names, one;

        while (rest.st_Len) {
            StrSplit(rest, '=', &one, &rest);
            if (StrSameNoCase(one, key))
                return (int)i;
        }
    }
    return -1;
}

/* A quoted run, from just past its '"': the closing quote is consumed. */
static bool quoted(struct parse *ps)
{
    for (;;) {
        char c = *ps->p++;

        if (!c)
            return false;
        if (c == '"')
            return true;
        if (c == '*' && *ps->p) {
            switch (c = *ps->p++) {
            case 'n': case 'N': c = '\n'; break;
            case 'e': case 'E': c = 0x1b; break;
            default: break;             /* *" and ** are themselves */
            }
        }
        *ps->text++ = c;
    }
}

/* The next word: 1, 0 at the end of the line, or an error. */
static int64_t next_word(struct parse *ps, struct word *w)
{
    while (space(*ps->p))
        ps->p++;
    if (!*ps->p)
        return 0;
    w->start = ps->p;
    w->text = ps->text;
    w->eq = 0;
    if (*ps->p == '"') {
        ps->p++;
        if (!quoted(ps))
            return why(ps, "a quote isn't closed", nullptr);
    } else {
        while (*ps->p && !space(*ps->p)) {
            char c = *ps->p++;

            *ps->text++ = c;
            if (c == '=' && !w->eq) {
                w->eq = (unsigned)(ps->text - w->text);
                if (*ps->p == '"') {    /* KEY="a value" */
                    ps->p++;
                    if (!quoted(ps))
                        return why(ps, "a quote isn't closed", nullptr);
                    break;
                }
            }
        }
    }
    *ps->text++ = '\0';
    return 1;
}

/* The rest of the line from 's', as it stands but its trailing spaces. */
static char *rest_of_line(struct parse *ps, const char *s)
{
    char *out = ps->text;
    unsigned n = length(s);

    while (n && space(s[n - 1]))
        n--;
    for (unsigned i = 0; i < n; i++)
        *ps->text++ = s[i];
    *ps->text++ = '\0';
    ps->p = s + length(s);
    return out;
}

static int64_t value(struct parse *ps, int64_t *array, unsigned i, char *text)
{
    struct item *it = &ps->item[i];
    int64_t v = (int64_t)text;

    if (it->flags & I_NUMBER) {
        struct Str s = StrFromC(text);
        bool neg = s.st_Len && s.st_Data[0] == '-';
        uint64_t u;

        if (!StrToU64(StrSlice(s, neg, s.st_Len), 10, &u) || u > (uint64_t)INT64_MAX + neg)
            return why(ps, "%S needs a number", it);
        *ps->number = neg ? (int64_t)(0 - u) : (int64_t)u;
        v = (int64_t)ps->number++;
    }
    if (it->flags & I_MULTI) {
        ps->multi[ps->multis++] = (char *)v;
    } else {
        if (it->given)
            return why(ps, "%S is given twice", it);
        array[i] = v;
    }
    it->given = true;
    return 0;
}

static int64_t parse_line(struct parse *ps, int64_t *array)
{
    struct word w;
    int64_t r;

    while ((r = next_word(ps, &w)) > 0) {
        int k = -1;

        if (*w.start != '"')
            k = keyword(ps, (struct Str){ w.text, w.eq ? w.eq - 1 : length(w.text) });
        if (k >= 0) {
            struct item *it = &ps->item[k];
            char *v = w.eq ? w.text + w.eq : nullptr;

            if (it->flags & I_SWITCH) {
                if (w.eq)
                    return why(ps, "%S takes no value", it);
                if (it->given)
                    return why(ps, "%S is given twice", it);
                array[k] = 1;
                it->given = true;
                continue;
            }
            if (it->flags & I_REST) {
                const char *s = w.eq ? w.start + w.eq : w.start + length(w.text);

                while (space(*s))
                    s++;
                if ((r = value(ps, array, (unsigned)k, rest_of_line(ps, s))))
                    return r;
                break;
            }
            if (!v || (!*v && w.start[w.eq] != '"')) {
                struct word next;

                if ((r = next_word(ps, &next)) < 0)
                    return r;
                if (!r)
                    return why(ps, "%S needs a value", it);
                v = next.text;
            }
            if ((r = value(ps, array, (unsigned)k, v)))
                return r;
            continue;
        }
        /* Positional: the first item it can fill. */
        for (k = 0; k < (int)ps->items; k++) {
            const struct item *it = &ps->item[k];

            if (!(it->flags & (I_KEYWORD | I_SWITCH)) && (!it->given || it->flags & I_MULTI))
                break;
        }
        if (k == (int)ps->items)
            return why(ps, "too many arguments", nullptr);
        if (ps->item[k].flags & I_REST) {
            if ((r = value(ps, array, (unsigned)k, rest_of_line(ps, w.start))))
                return r;
            break;
        }
        if ((r = value(ps, array, (unsigned)k, w.text)))
            return r;
    }
    return r < 0 ? r : 0;
}

/* The /M's words to the /A items after it that weren't given, from its
 * end; then every /A must be there. */
static int64_t finish(struct parse *ps, int64_t *array)
{
    if (ps->multi_item >= 0) {
        unsigned need = 0, give;
        struct item *m = &ps->item[ps->multi_item];

        for (unsigned i = (unsigned)ps->multi_item + 1; i < ps->items; i++)
            need += (ps->item[i].flags & (I_REQUIRED | I_KEYWORD | I_SWITCH)) == I_REQUIRED &&
                    !ps->item[i].given;
        give = need < ps->multis ? need : ps->multis;
        ps->multis -= give;
        for (unsigned i = (unsigned)ps->multi_item + 1, from = ps->multis; give; i++) {
            struct item *it = &ps->item[i];

            if ((it->flags & (I_REQUIRED | I_KEYWORD | I_SWITCH)) != I_REQUIRED || it->given)
                continue;
            array[i] = (int64_t)ps->multi[from++];
            it->given = true;
            give--;
        }
        m->given = ps->multis > 0;
        ps->multi[ps->multis] = nullptr;
        if (m->given)
            array[ps->multi_item] = (int64_t)ps->multi;
    }
    for (unsigned i = 0; i < ps->items; i++)
        if (ps->item[i].flags & I_REQUIRED && !ps->item[i].given)
            return why(ps, "%S is required", &ps->item[i]);
    return 0;
}

int64_t ReadArgs(const char *argTemplate, int64_t *array, struct RDArgs *rdArgs)
{
    struct parse ps = { .ra = rdArgs };
    const char *line;
    uint64_t len, words, size;
    uint8_t *block;
    int64_t r;

    if (!rdArgs)
        return ERR_BADARG;
    rdArgs->rda_Buffer = nullptr;
    rdArgs->rda_Why[0] = '\0';
    line = rdArgs->rda_Source ? rdArgs->rda_Source : "";
    if (!parse_template(&ps, argTemplate))
        return why(&ps, "the command's template is wrong", nullptr);
    if (StrSame(StrTrim(StrFromC(line)), STR("?")))
        return ERR_BADARG;              /* rda_Why "": show the template */

    /* Each word is a byte of the line and a space at least, and its
     * copy no longer, with a NUL. A /F's copy may cover the line again. */
    len = length(line);
    words = len / 2 + 2;
    size = 2 * len + words + 8;
    if (!(block = AllocVec((ITEMS + words) * sizeof(int64_t) + (words + 1) * sizeof(char *) + size,
                           0)))
        return ERR_NOMEM;
    ps.p = line;
    ps.number = (int64_t *)block;
    ps.multi = (char **)(ps.number + ITEMS + words);
    ps.text = (char *)(ps.multi + words + 1);   /* last, so a sanitizer sees past it */
    if ((r = parse_line(&ps, array)) || (r = finish(&ps, array))) {
        FreeVec(block);
        return r;
    }
    rdArgs->rda_Buffer = block;
    return 0;
}

void FreeArgs(struct RDArgs *rdArgs)
{
    if (rdArgs) {
        FreeVec(rdArgs->rda_Buffer);
        rdArgs->rda_Buffer = nullptr;
    }
}

/* The Shell's line: its directory (and a tab and its console, a window's), a
 * newline, then the arguments. */
void SplitArgs(uint64_t arg, char *cwd, unsigned cwdSize, const char **args)
{
    const char *line = arg ? (const char *)arg : "";
    unsigned n = 0;

    *args = line;
    cwd[0] = '\0';
    for (const char *p = line; *p; p++)
        if (*p == '\n') {
            const char *q = line, *tab = nullptr;
            char con[CON_NAME];
            unsigned k = 0;

            for (; q < p && *q != '\t' && n < cwdSize - 1; q++)
                cwd[n++] = *q;
            cwd[n] = '\0';
            for (const char *t = line; t < p; t++)
                if (*t == '\t' && !tab)
                    tab = t;
            if (tab) {                  /* the Shell's console, a window's */
                for (q = tab + 1; q < p && k < CON_NAME - 1; q++)
                    con[k++] = *q;
                con[k] = '\0';
                SetConName(con);
            }
            *args = p + 1;
            break;
        }
    if (!cwd[0] && cwdSize > 4) {
        cwd[0] = 'S', cwd[1] = 'Y', cwd[2] = 'S', cwd[3] = ':', cwd[4] = '\0';
    }
}
