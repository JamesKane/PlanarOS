/*
 * drv/xhci/hid.c -- a mouse's reports, by its report descriptor
 * (HID 1.11 §6.2.2)
 *
 * A mouse is read in the report protocol, as its report descriptor says
 * its reports are: some ignore SET_PROTOCOL's boot protocol and send
 * their own (the CIX's Corsair, 1b1c:1bac, whose Y input read as a boot
 * mouse's was X's high byte). hid_layout finds the input report with
 * Generic Desktop's X and Y (0x30, 0x31), and where they and the Button
 * page's first three lie in it; hid_boot turns each such report into
 * the boot mouse's three bytes input reads (B.2), X and Y clamped to a
 * signed byte. Push, Pop, delimiters and long items are passed over.
 */
#include "xhci.h"

constexpr uint32_t USAGE_X = 0x10030, USAGE_Y = 0x10031, PAGE_BUTTON = 9;
constexpr unsigned USAGES = 16;

bool hid_layout(const uint8_t *d, uint32_t n, struct hid_layout *l)
{
    uint32_t page = 0, size = 0, count = 0, id = 0;
    uint32_t usage[USAGES], nusage = 0, umin = 0;
    uint16_t off[256] = { 0 };          /* bits so far in each report ID's input report */
    uint16_t button[256][3] = { 0 };    /* each report ID's buttons 1 to 3: their bit + 1, 0 none */
    bool range = false;

    *l = (struct hid_layout){ 0 };
    for (uint32_t i = 0; i < n;) {
        uint8_t b = d[i];
        uint32_t sz = (b & 3) == 3 ? 4 : b & 3, v = 0;

        if (b == 0xfe) {                /* a long item */
            i += 3 + (i + 1 < n ? d[i + 1] : 0);
            continue;
        }
        if (i + 1 + sz > n)
            break;
        for (uint32_t k = 0; k < sz; k++)
            v |= (uint32_t)d[i + 1 + k] << 8 * k;
        i += 1 + sz;
        switch (b & 0xfc) {
        case 0x80:                      /* Input */
            for (uint32_t f = 0; f < count; f++) {
                uint32_t u = range ? umin + f : nusage ? usage[f < nusage ? f : nusage - 1] : 0;
                uint32_t at = off[id] + f * size;

                if (u <= 0xffff)
                    u |= page << 16;
                if (v & 1 || (l->has_x && id != l->id))
                    continue;           /* constant (padding), or another report's */
                if (u == USAGE_X && !l->has_x && size <= 32) {
                    l->has_x = true, l->id = (uint8_t)id, l->x_at = (uint16_t)at, l->x_bits = (uint8_t)size;
                } else if (u == USAGE_Y && !l->has_y && size <= 32) {
                    l->has_y = true, l->y_at = (uint16_t)at, l->y_bits = (uint8_t)size;
                } else if (u >> 16 == PAGE_BUTTON && (u & 0xffff) >= 1 && (u & 0xffff) <= 3 && size == 1) {
                    button[id][(u & 0xffff) - 1] = (uint16_t)(at + 1);
                }
            }
            off[id] = (uint16_t)(off[id] + size * count);
            nusage = 0, range = false;
            break;
        case 0x90: case 0xb0: case 0xa0: case 0xc0:     /* Output, Feature, Collection, End */
            nusage = 0, range = false;
            break;
        case 0x04:                      /* Usage Page */
            page = v;
            break;
        case 0x74:                      /* Report Size */
            size = v;
            break;
        case 0x94:                      /* Report Count */
            count = v;
            break;
        case 0x84:                      /* Report ID */
            id = v & 0xff;
            l->ids = true;
            break;
        case 0x08:                      /* Usage */
            if (nusage < USAGES)
                usage[nusage++] = v;
            break;
        case 0x18:                      /* Usage Minimum */
            umin = v;
            range = true;
            break;
        }
    }
    for (unsigned k = 0; k < 3; k++)
        l->button_at[k] = button[l->id][k];
    return l->has_x && l->has_y;
}

static int32_t field(const uint8_t *r, uint32_t len, uint32_t at, uint32_t bits, bool sign)
{
    uint64_t v = 0;

    if (at + bits > len * 8)
        return 0;
    for (uint32_t k = 0; k < bits; k++)
        v |= (uint64_t)(r[(at + k) / 8] >> (at + k) % 8 & 1) << k;
    if (sign && bits && v >> (bits - 1) & 1)
        v |= ~0ull << bits;
    return (int32_t)(int64_t)v;
}

bool hid_boot(const struct hid_layout *l, const uint8_t *r, uint32_t len, uint8_t out[3])
{
    int32_t x, y;

    if (l->ids) {
        if (len < 1 || r[0] != l->id)
            return false;
        r++;
        len--;
    }
    out[0] = 0;
    for (unsigned b = 0; b < 3; b++)
        if (l->button_at[b] && field(r, len, l->button_at[b] - 1u, 1, false))
            out[0] |= (uint8_t)(1u << b);
    x = field(r, len, l->x_at, l->x_bits, true);
    y = field(r, len, l->y_at, l->y_bits, true);
    out[1] = (uint8_t)(int8_t)(x < -127 ? -127 : x > 127 ? 127 : x);
    out[2] = (uint8_t)(int8_t)(y < -127 ? -127 : y > 127 ? 127 : y);
    return true;
}
