/*
 * drv/display/font8x8.h -- 8x8 console font for the framebuffer
 */
#ifndef DISPLAY_FONT8X8_H
#define DISPLAY_FONT8X8_H

#include <stdint.h>

constexpr unsigned FONT8X8_FIRST = 0x20;
constexpr unsigned FONT8X8_COUNT = 0x7f - FONT8X8_FIRST;

extern const uint8_t font8x8[FONT8X8_COUNT][8];

#endif /* DISPLAY_FONT8X8_H */
