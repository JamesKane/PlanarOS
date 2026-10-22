/*
 * planar/utility.h -- utility.library (docs §17.4.3)
 *
 * Commodore's V36-V39 utility.library: tag lists, hooks, case-blind
 * comparison and SNPrintf. Tags carry 64 bits. Then Planar's addition:
 * Span and Str, a pointer and a length passed in two registers, and
 * readers that check every offset against the end, with stdckdint.h,
 * and fail rather than read past it. Nothing here keeps state.
 */
#ifndef PLANAR_UTILITY_H
#define PLANAR_UTILITY_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include <planar/exec.h>

/* --- Tag lists (V36) --- */

typedef uint64_t Tag;

struct TagItem {
    Tag ti_Tag;
    uint64_t ti_Data;
};

constexpr Tag TAG_DONE   = 0;           /* the list ends */
constexpr Tag TAG_END    = 0;
constexpr Tag TAG_IGNORE = 1;           /* this item is skipped */
constexpr Tag TAG_MORE   = 2;           /* ti_Data: the list goes on there */
constexpr Tag TAG_SKIP   = 3;           /* this item and the next ti_Data */
constexpr Tag TAG_USER   = 1ULL << 31;  /* a program's own tags from here */

/*
 * The next item that isn't a control tag, following TAG_MORE and
 * stepping over TAG_IGNORE and TAG_SKIP; nullptr at the end, and after.
 * Advances *tagListPtr.
 */
struct TagItem *NextTagItem(const struct TagItem **tagListPtr);
struct TagItem *FindTagItem(Tag tagValue, const struct TagItem *tagList);
uint64_t GetTagData(Tag tagValue, uint64_t defaultVal, const struct TagItem *tagList);
bool TagInArray(Tag tagValue, const Tag *tagArray);        /* ends with TAG_DONE */

/* --- Hooks (V36) --- */

struct Hook {
    uint64_t (*h_Entry)(struct Hook *hook, void *object, void *message);
    uint64_t (*h_SubEntry)(struct Hook *hook, void *object, void *message);
    void *h_Data;
};

uint64_t CallHookPkt(struct Hook *hook, void *object, void *paramPacket);

/*
 * --- Strings and formatting (V36, V37) ---
 *
 * Case is ASCII's: a byte above 0x7f is left alone, so UTF-8 passes
 * through whole (Commodore's were ISO 8859-1's, which would change
 * UTF-8's lead bytes). Comparison is of the bytes as unsigned.
 * VSNPrintf writes RawDoFmt's output to the buffer, cut to fit and
 * always ended with a NUL if bufferSize isn't 0, and returns what the
 * whole would have been, as C's vsnprintf does: under bufferSize if it
 * fit.
 */
int ToUpper(int c);
int ToLower(int c);
int Stricmp(const char *s1, const char *s2);
int Strnicmp(const char *s1, const char *s2, uint64_t n);
int64_t VSNPrintf(char *buffer, uint64_t bufferSize, const char *fmt, va_list args);
int64_t SNPrintf(char *buffer, uint64_t bufferSize, const char *fmt, ...);

/* --- Span and Str --- */

/* Bytes by their length: often a view into a read-only Area. */
struct Span {
    const uint8_t *sp_Data;
    uint64_t sp_Len;
};

/* A string by its length: no NUL, so a slice of something read-only. */
struct Str {
    const char *st_Data;
    uint64_t st_Len;
};

#define STR(lit)    ((struct Str){ .st_Data = (lit), .st_Len = sizeof(lit) - 1 })

struct Str StrFromC(const char *s);         /* nullptr: the empty Str */
struct Str StrSlice(struct Str s, uint64_t start, uint64_t end);   /* clamped to s */
struct Str StrTrim(struct Str s);           /* spaces, tabs, CR and LF, both ends */
/* At the first 'sep': what's before it and after it. False, with
 * *head = s and *tail empty, if there's none. */
bool StrSplit(struct Str s, char sep, struct Str *head, struct Str *tail);
bool StrSame(struct Str a, struct Str b);
bool StrSameNoCase(struct Str a, struct Str b);
bool StrHasPrefix(struct Str s, struct Str prefix);
/* All of 's' a number in 'base' (2 to 36), or base 0: 0x, 0b or 0o
 * says, else decimal. False if empty, a digit is wrong or it overflows. */
bool StrToU64(struct Str s, unsigned base, uint64_t *out);

/*
 * Checked slicing and loads. Each is false, and leaves *out alone, if
 * offset and length run past the span's end, however large they are.
 * Byte loads: any alignment, any memory.
 */
bool SpanSlice(struct Span s, uint64_t offset, uint64_t len, struct Span *out);
bool SpanReadU8(struct Span s, uint64_t offset, uint8_t *out);
bool SpanReadU16LE(struct Span s, uint64_t offset, uint16_t *out);
bool SpanReadU32LE(struct Span s, uint64_t offset, uint32_t *out);
bool SpanReadU64LE(struct Span s, uint64_t offset, uint64_t *out);
bool SpanReadU16BE(struct Span s, uint64_t offset, uint16_t *out);
bool SpanReadU32BE(struct Span s, uint64_t offset, uint32_t *out);
bool SpanReadU64BE(struct Span s, uint64_t offset, uint64_t *out);

/*
 * utility.library's "main" interface, version 1 (V6 step 4): every call
 * above, in this order, which is the ABI. A program opens it:
 *
 *     struct Library *lib = IExec->OpenLibrary("utility.library", 6);
 *     struct UtilityIFace *IUtility =
 *         lib ? (struct UtilityIFace *)IExec->GetInterface(lib, "main", 1) : nullptr;
 */
struct UtilityIFace {
    struct Interface Data;

    struct TagItem *(*NextTagItem)(const struct TagItem **tagListPtr);
    struct TagItem *(*FindTagItem)(Tag tagValue, const struct TagItem *tagList);
    uint64_t (*GetTagData)(Tag tagValue, uint64_t defaultVal, const struct TagItem *tagList);
    bool (*TagInArray)(Tag tagValue, const Tag *tagArray);
    uint64_t (*CallHookPkt)(struct Hook *hook, void *object, void *paramPacket);

    int (*ToUpper)(int c);
    int (*ToLower)(int c);
    int (*Stricmp)(const char *s1, const char *s2);
    int (*Strnicmp)(const char *s1, const char *s2, uint64_t n);
    int64_t (*VSNPrintf)(char *buffer, uint64_t bufferSize, const char *fmt, va_list args);
    int64_t (*SNPrintf)(char *buffer, uint64_t bufferSize, const char *fmt, ...);

    struct Str (*StrFromC)(const char *s);
    struct Str (*StrSlice)(struct Str s, uint64_t start, uint64_t end);
    struct Str (*StrTrim)(struct Str s);
    bool (*StrSplit)(struct Str s, char sep, struct Str *head, struct Str *tail);
    bool (*StrSame)(struct Str a, struct Str b);
    bool (*StrSameNoCase)(struct Str a, struct Str b);
    bool (*StrHasPrefix)(struct Str s, struct Str prefix);
    bool (*StrToU64)(struct Str s, unsigned base, uint64_t *out);

    bool (*SpanSlice)(struct Span s, uint64_t offset, uint64_t len, struct Span *out);
    bool (*SpanReadU8)(struct Span s, uint64_t offset, uint8_t *out);
    bool (*SpanReadU16LE)(struct Span s, uint64_t offset, uint16_t *out);
    bool (*SpanReadU32LE)(struct Span s, uint64_t offset, uint32_t *out);
    bool (*SpanReadU64LE)(struct Span s, uint64_t offset, uint64_t *out);
    bool (*SpanReadU16BE)(struct Span s, uint64_t offset, uint16_t *out);
    bool (*SpanReadU32BE)(struct Span s, uint64_t offset, uint32_t *out);
    bool (*SpanReadU64BE)(struct Span s, uint64_t offset, uint64_t *out);
};

#endif /* PLANAR_UTILITY_H */
