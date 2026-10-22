/*
 * servers/intuition/pointer.h -- the pointer's sprite (v7-plan step 3)
 *
 * 16 by 16 ARGB, its hot spot the top left: §13.2's cursor.fill, top to
 * bottom from #E02828 to #A81818, in a white outline. Every pixel is
 * opaque or clear, so it lands on the screen as it is, whatever blends
 * it: conformance/window.c reads it back. Shared with window.c.
 */
#ifndef SERVERS_INTUITION_POINTER_H
#define SERVERS_INTUITION_POINTER_H

#include <stdint.h>

constexpr uint32_t POINTER_SIZE = 16;

static inline uint32_t pointer_pixel(uint32_t x, uint32_t y)
{
    static const char shape[16][17] = {
        "X...............", "XX..............", "XRX.............", "XRRX............",
        "XRRRX...........", "XRRRRX..........", "XRRRRRX.........", "XRRRRRRX........",
        "XRRRRRRRX.......", "XRRRRRRRRX......", "XRRRRRXXXXX.....", "XRRXRRX.........",
        "XRX.XRRX........", "XX..XRRX........", "X....XRRX.......", ".....XXXX.......",
    };
    char c = x < 16 && y < 16 ? shape[y][x] : '.';

    if (c == 'X')
        return 0xffffffffu;
    if (c == 'R')       /* 0xe0 to 0xa8 red, 0x28 to 0x18 green and blue */
        return 0xff000000u | (0xe0u - y * 56 / 15) << 16 | (0x28u - y * 16 / 15) * 0x101u;
    return 0;
}

#endif /* SERVERS_INTUITION_POINTER_H */
