/*
 * clib/stdbit.h -- C23's bit and byte functions (docs §17.4.5)
 *
 * Each by Clang's type-generic builtins (__builtin_clzg, _ctzg and
 * _popcountg), which give a result for 0 too, so nothing here branches
 * but first_* and bit_ceil. Inline, typed _uc to _ull, and type-generic
 * without a suffix for the five standard unsigned types.
 */
#ifndef CLIB_STDBIT_H
#define CLIB_STDBIT_H

#define __STDC_VERSION_STDBIT_H__   202311L
#define __STDC_ENDIAN_LITTLE__      __ORDER_LITTLE_ENDIAN__
#define __STDC_ENDIAN_BIG__         __ORDER_BIG_ENDIAN__
#define __STDC_ENDIAN_NATIVE__      __BYTE_ORDER__

#define __CLIB_STDBIT(S, T)                                                                     \
    static inline unsigned int stdc_leading_zeros_##S(T x)                                     \
    { return (unsigned)__builtin_clzg(x, (int)(sizeof(T) * __CHAR_BIT__)); }                   \
    static inline unsigned int stdc_leading_ones_##S(T x)                                      \
    { return stdc_leading_zeros_##S((T)~x); }                                                  \
    static inline unsigned int stdc_trailing_zeros_##S(T x)                                    \
    { return (unsigned)__builtin_ctzg(x, (int)(sizeof(T) * __CHAR_BIT__)); }                   \
    static inline unsigned int stdc_trailing_ones_##S(T x)                                     \
    { return stdc_trailing_zeros_##S((T)~x); }                                                 \
    static inline unsigned int stdc_first_leading_zero_##S(T x)                                \
    { return (T)~x ? stdc_leading_ones_##S(x) + 1 : 0; }                                       \
    static inline unsigned int stdc_first_leading_one_##S(T x)                                 \
    { return x ? stdc_leading_zeros_##S(x) + 1 : 0; }                                          \
    static inline unsigned int stdc_first_trailing_zero_##S(T x)                               \
    { return (T)~x ? stdc_trailing_ones_##S(x) + 1 : 0; }                                      \
    static inline unsigned int stdc_first_trailing_one_##S(T x)                                \
    { return x ? stdc_trailing_zeros_##S(x) + 1 : 0; }                                         \
    static inline unsigned int stdc_count_ones_##S(T x)                                        \
    { return (unsigned)__builtin_popcountg(x); }                                               \
    static inline unsigned int stdc_count_zeros_##S(T x)                                       \
    { return stdc_count_ones_##S((T)~x); }                                                     \
    static inline _Bool stdc_has_single_bit_##S(T x)                                           \
    { return stdc_count_ones_##S(x) == 1; }                                                    \
    static inline unsigned int stdc_bit_width_##S(T x)                                         \
    { return (unsigned)(sizeof(T) * __CHAR_BIT__) - stdc_leading_zeros_##S(x); }               \
    static inline T stdc_bit_floor_##S(T x)                                                    \
    { return x ? (T)((T)1 << (stdc_bit_width_##S(x) - 1)) : 0; }                               \
    /* 0 when it doesn't fit, as C23 allows */                                                 \
    static inline T stdc_bit_ceil_##S(T x)                                                     \
    {                                                                                          \
        unsigned w = x > 1 ? stdc_bit_width_##S((T)(x - 1)) : 0;                               \
        return w < sizeof(T) * __CHAR_BIT__ ? (T)((T)1 << w) : 0;                              \
    }

__CLIB_STDBIT(uc, unsigned char)
__CLIB_STDBIT(us, unsigned short)
__CLIB_STDBIT(ui, unsigned int)
__CLIB_STDBIT(ul, unsigned long)
__CLIB_STDBIT(ull, unsigned long long)

#undef __CLIB_STDBIT

#define __CLIB_STDBIT_GENERIC(f, x)                                                            \
    _Generic((x),                                                                              \
        unsigned char: f##_uc, unsigned short: f##_us, unsigned int: f##_ui,                   \
        unsigned long: f##_ul, unsigned long long: f##_ull)(x)

#define stdc_leading_zeros(x)       __CLIB_STDBIT_GENERIC(stdc_leading_zeros, x)
#define stdc_leading_ones(x)        __CLIB_STDBIT_GENERIC(stdc_leading_ones, x)
#define stdc_trailing_zeros(x)      __CLIB_STDBIT_GENERIC(stdc_trailing_zeros, x)
#define stdc_trailing_ones(x)       __CLIB_STDBIT_GENERIC(stdc_trailing_ones, x)
#define stdc_first_leading_zero(x)  __CLIB_STDBIT_GENERIC(stdc_first_leading_zero, x)
#define stdc_first_leading_one(x)   __CLIB_STDBIT_GENERIC(stdc_first_leading_one, x)
#define stdc_first_trailing_zero(x) __CLIB_STDBIT_GENERIC(stdc_first_trailing_zero, x)
#define stdc_first_trailing_one(x)  __CLIB_STDBIT_GENERIC(stdc_first_trailing_one, x)
#define stdc_count_ones(x)          __CLIB_STDBIT_GENERIC(stdc_count_ones, x)
#define stdc_count_zeros(x)         __CLIB_STDBIT_GENERIC(stdc_count_zeros, x)
#define stdc_has_single_bit(x)      __CLIB_STDBIT_GENERIC(stdc_has_single_bit, x)
#define stdc_bit_width(x)           __CLIB_STDBIT_GENERIC(stdc_bit_width, x)
#define stdc_bit_floor(x)           __CLIB_STDBIT_GENERIC(stdc_bit_floor, x)
#define stdc_bit_ceil(x)            __CLIB_STDBIT_GENERIC(stdc_bit_ceil, x)

#endif /* CLIB_STDBIT_H */
