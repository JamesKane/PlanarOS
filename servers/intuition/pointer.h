/*
 * servers/intuition/pointer.h -- the pointer's sprite (v7-plan step 3)
 *
 * 16 by 16 ARGB, its hot spot the top left: §13.2's cursor.fill, top to
 * bottom from #E02828 to #A81818, in a white outline. Every pixel is
 * opaque or clear, so it lands on the screen as it is, whatever blends
 * it: conformance/window.c reads it back. Shared with window.c.
 *
 * On a screen of 1600 by 900 or more it's drawn twice the size, each
 * pixel 2 by 2 (V7 step 6: 16 was small on 1920 by 1080). The hot spot
 * still reaches the screen's last pixel, so at its right and bottom
 * edges only the tip shows, as on other desktops.
 */
#ifndef SERVERS_INTUITION_POINTER_H
#define SERVERS_INTUITION_POINTER_H

#include <stdint.h>

constexpr uint32_t POINTER_SIZE = 16;

static inline uint32_t pointer_scale(uint32_t width, uint32_t height)
{
    return width >= 1600 && height >= 900 ? 2 : 1;
}

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
