/*
 * clib/assert.h -- C's assert (docs §17.4.5)
 *
 * No include guard: each inclusion redefines assert by NDEBUG, as C
 * says. A failed assertion is logged, with where, and ends the process
 * as abort does.
 */
#undef assert
#ifdef NDEBUG
#define assert(...) ((void)0)
#else
#define assert(...) \
    ((__VA_ARGS__) ? (void)0 : __clib_assert(#__VA_ARGS__, __FILE__, __LINE__, __func__))
#endif

#if !defined(static_assert) && __STDC_VERSION__ < 202311L
#define static_assert _Static_assert
#endif

__attribute__((noreturn)) void __clib_assert(const char *expr, const char *file, int line,
                                             const char *func);
