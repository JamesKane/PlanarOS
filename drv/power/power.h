/*
 * drv/power/power.h -- the power server's parts (planar/devices/power.h)
 *
 * core.c keeps the resources and the votes; a backend per kind of
 * hardware (k1.c, scmi.c, model.c) says what resources there are, turns
 * each on and off, and reads it back. main.c is the driver around them,
 * and the only part that makes kernel calls: the rest also runs on the
 * host, against made-up registers (test.c).
 */
#ifndef DRV_POWER_H
#define DRV_POWER_H

#include <stddef.h>
#include <stdint.h>

#include <planar/devices/power.h>

constexpr unsigned PW_RES = 96;         /* resources, all backends together */
constexpr unsigned PW_VOTES = 96;
constexpr unsigned PW_DEVICES = 8;      /* of the server's, a backend's */

struct pw_res {
    const char *name;
    uint8_t kind;                   /* POWER_DOMAIN, POWER_CLOCK, POWER_RESET */
    bool on;                        /* a reset is on when released */
    bool kept;                      /* on when the server started: the firmware's */
    uint16_t votes;
    uint32_t a, b, c;               /* the backend's: where it is */
    const struct pw_backend *be;
};

/* The server's devices, as hwdb gave them. */
struct pw_dev {
    const char *key;
    volatile uint8_t *regs;         /* mapped at its base; nullptr if it has none */
    uint64_t base, size;
    const struct pw_backend *be;
};

struct pw_backend {
    const char *name;
    const char *const *keys;        /* the device keys it takes; ends with nullptr */
    /* Its resources, added with pw_add once all its devices are known;
     * false (having said why) if its hardware can't be used. */
    bool (*init)(void);
    /* The resource a reference names, or -1 if it leaves that one as it is. */
    int (*xlate)(unsigned dev, const struct PowerRef *r);
    /* Turn it on or off: 0, or an error (ERR_TIMEOUT). */
    int64_t (*set)(struct pw_res *r, bool on);
    /* Is it on? 1, 0, or an error. */
    int64_t (*get)(struct pw_res *r);
    /* Named sets: each a name, then resource names; nullptr ends both. */
    const char *const *const *sets;
};

/* --- core.c --- */

extern struct pw_res pw_res[PW_RES];
extern unsigned pw_nres;
extern struct pw_dev pw_dev[PW_DEVICES];
extern unsigned pw_ndev;

/* A backend adds its resources; their index, or -1 when full. */
int pw_add(const struct pw_backend *be, const char *name, uint8_t kind, uint32_t a, uint32_t b,
           uint32_t c);
int pw_find(const char *name);

/* Devices, then backends: each device's key picks its backend; each
 * backend with a device runs init, and every resource it adds is read:
 * what's on is kept. False if no backend took any device. */
void pw_device(const char *key, volatile uint8_t *regs, uint64_t base, uint64_t size);
bool pw_start(void);

/* One message, from client 'client' (a number that stays its own while
 * it's connected): read, acted on and answered in place. */
void pw_message(unsigned client, struct PowerMsg *m);
/* The client went: its votes go. */
void pw_gone(unsigned client);

/* Votes standing, for the report and the tests; and back to no devices,
 * no resources and no votes (the host's tests, between backends). */
unsigned pw_votes(void);
void pw_reset(void);

/* The model's switches, in the order it was told (for the tests): each
 * resource's index in it, plus 0x80 for on. */
extern uint8_t pw_model_trace[32];
extern unsigned pw_model_ntrace;

/* --- What main.c (or the host's test.c) provides. --- */

void pw_delay_us(uint64_t us);
uint64_t pw_now_us(void);
void pw_say(const char *text);
/* The firmware call hwdb allowed the server's lease (CallLease): x[0]
 * is the function on the way in, what it returned on the way out. 0, or
 * an error. */
int64_t pw_firmware_call(uint64_t x[4]);
/* Where [base, base + size) is mapped, if a range of the server's lease
 * holds it (a channel that's no device's registers); nullptr if not. */
volatile uint8_t *pw_map(uint64_t base, uint64_t size);

/* Poll until (*reg & mask) == want, at most 'us': true if it came. */
bool pw_poll32(volatile uint32_t *reg, uint32_t mask, uint32_t want, uint64_t us);

/* --- The backends. --- */

extern const struct pw_backend pw_k1, pw_scmi, pw_model;

#endif /* DRV_POWER_H */
