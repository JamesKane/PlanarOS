/*
 * lib/gfx/font8x8.h -- the 8x8 font: gfx.library's Text, and display's
 * console, which links the same table (V7 step 2)
 */
#ifndef LIB_GFX_FONT8X8_H
#define LIB_GFX_FONT8X8_H

#include <stdint.h>

constexpr unsigned FONT8X8_FIRST = 0x20;
constexpr unsigned FONT8X8_COUNT = 0x7f - FONT8X8_FIRST;

extern const uint8_t font8x8[FONT8X8_COUNT][8];

#endif /* LIB_GFX_FONT8X8_H */
