/*
 * drv/power/model.c -- a power controller with nothing behind it
 *
 * QEMU's machines have nothing to power, so hwdb's test mode adds a node
 * "planar,power-model" there, with no registers, and has the RTC need
 * its domain 1 (servers/hwdb/server.c). The model's resources are bits
 * in this file: domain 0 starts on (the firmware's), the rest off, and
 * domain 3 never comes on, for the error paths. Its "gpu" set is what a
 * test votes for, as a GPU server will on a board; "broken" turns domain
 * 1 on and then fails on domain 3, so the vote must undo the first.
 */
#include <planar/syscalls.h>

#include "power.h"

enum { DOMAINS = 4, CLOCKS = 2, RESETS = 1 };

static const char *const names[] = {
    "model.domain0", "model.domain1", "model.domain2", "model.domain3",
    "model.clock0", "model.clock1", "model.reset0",
};
static bool state[DOMAINS + CLOCKS + RESETS];
static int first = -1;              /* pw_res index of names[0] */

uint8_t pw_model_trace[32];
unsigned pw_model_ntrace;

static bool init(void)
{
    for (unsigned i = 0; i < sizeof state / sizeof state[0]; i++)
        state[i] = i == 0;
    first = -1;
    for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++) {
        uint8_t kind = i < DOMAINS ? POWER_DOMAIN : i < DOMAINS + CLOCKS ? POWER_CLOCK : POWER_RESET;
        int r = pw_add(&pw_model, names[i], kind, i, 0, 0);

        if (r < 0)
            return false;
        if (first < 0)
            first = r;
    }
    return true;
}

static int xlate(unsigned, const struct PowerRef *r)
{
    uint32_t base = r->pr_Kind == POWER_DOMAIN ? 0 : r->pr_Kind == POWER_CLOCK ? DOMAINS
                                                                              : DOMAINS + CLOCKS;
    uint32_t count = r->pr_Kind == POWER_DOMAIN ? DOMAINS : r->pr_Kind == POWER_CLOCK ? CLOCKS
                                                                                     : RESETS;

    if (r->pr_NCells != 1 || r->pr_Cells[0] >= count)
        return -1;
    return first + (int)(base + r->pr_Cells[0]);
}

static int64_t set(struct pw_res *r, bool on)
{
    if (r->a == 3 && on)
        return ERR_TIMEOUT;
    state[r->a] = on;
    if (pw_model_ntrace < sizeof pw_model_trace)
        pw_model_trace[pw_model_ntrace++] = (uint8_t)(r->a | (on ? 0x80 : 0));
    return 0;
}

static int64_t get(struct pw_res *r)
{
    return state[r->a];
}

static const char *const keys[] = { "planar,power-model", nullptr };
static const char *const gpu[] = { "gpu", "model.domain2", "model.clock1", "model.reset0", nullptr };
static const char *const broken[] = { "broken", "model.domain1", "model.domain3", nullptr };
static const char *const *const sets[] = { gpu, broken, nullptr };

const struct pw_backend pw_model = {
    .name = "model", .keys = keys, .init = init, .xlate = xlate, .set = set, .get = get,
    .sets = sets,
};
