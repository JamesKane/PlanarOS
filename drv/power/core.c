/*
 * drv/power/core.c -- resources and votes (planar/devices/power.h)
 *
 * A resource is on while any vote holds it. A vote turns its resources
 * on in kind order (domains, clocks, resets released, then supplies), and if one
 * fails, turns off again what it had turned on and answers the error:
 * a vote is all of its set or none. Off goes the other way round, and
 * only for what the server itself turned on (pw_res.kept).
 *
 * What's on when the server starts is kept, whichever start it is. On
 * the first that's what the firmware left on, which something nobody
 * votes for may need (the console's clock). After a restart it may also
 * be what a vote of the last server turned on: hwdb votes again for
 * every driver, but a resource now kept stays on once they've gone. That
 * wastes power and breaks nothing.
 */
#include <planar/syscalls.h>

#include "power.h"

struct pw_res pw_res[PW_RES];
unsigned pw_nres;
struct pw_dev pw_dev[PW_DEVICES];
unsigned pw_ndev;

struct vote {
    bool used;
    uint8_t n;
    uint16_t client;
    uint32_t tag;
    uint16_t res[POWER_REFS];       /* in the order they go on */
};

static struct vote votes[PW_VOTES];

static bool streq(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return *a == *b;
}

int pw_add(const struct pw_backend *be, const char *name, uint8_t kind, uint32_t a, uint32_t b,
           uint32_t c)
{
    if (pw_nres == PW_RES)
        return -1;
    pw_res[pw_nres] = (struct pw_res){ .name = name, .kind = kind, .a = a, .b = b, .c = c, .be = be };
    return (int)pw_nres++;
}

int pw_find(const char *name)
{
    for (unsigned i = 0; i < pw_nres; i++)
        if (streq(pw_res[i].name, name))
            return (int)i;
    return -1;
}

static const struct pw_backend *const backends[] = { &pw_k1, &pw_scmi, &pw_model };

void pw_device(const char *key, volatile uint8_t *regs, uint64_t base, uint64_t size)
{
    struct pw_dev *d;

    if (pw_ndev == PW_DEVICES)
        return;
    d = &pw_dev[pw_ndev++];
    *d = (struct pw_dev){ .key = key, .regs = regs, .base = base, .size = size };
    for (unsigned b = 0; b < sizeof backends / sizeof backends[0] && !d->be; b++)
        for (const char *const *k = backends[b]->keys; *k; k++)
            if (streq(*k, key))
                d->be = backends[b];
}

bool pw_start(void)
{
    bool any = false;

    for (unsigned b = 0; b < sizeof backends / sizeof backends[0]; b++) {
        bool has = false;

        for (unsigned i = 0; i < pw_ndev; i++)
            has |= pw_dev[i].be == backends[b];
        if (has && backends[b]->init())
            any = true;
    }
    for (unsigned i = 0; i < pw_nres; i++) {
        int64_t on = pw_res[i].be->get(&pw_res[i]);

        /* One that can't be read counts as the firmware's: never turned off. */
        pw_res[i].on = on != 0;
        pw_res[i].kept = on != 0;
    }
    return any;
}

void pw_reset(void)
{
    pw_nres = pw_ndev = 0;
    for (unsigned i = 0; i < PW_VOTES; i++)
        votes[i].used = false;
}

unsigned pw_votes(void)
{
    unsigned n = 0;

    for (unsigned i = 0; i < PW_VOTES; i++)
        n += votes[i].used;
    return n;
}

/* --- One resource. --- */

static int64_t take(unsigned i, bool *turned)
{
    struct pw_res *r = &pw_res[i];
    int64_t e;

    *turned = false;
    if (!r->on) {
        if ((e = r->be->set(r, true)) != 0)
            return e;
        r->on = true;
        *turned = true;
    }
    r->votes++;
    return 0;
}

static void drop(unsigned i)
{
    struct pw_res *r = &pw_res[i];

    if (r->votes && --r->votes == 0 && r->on && !r->kept && r->be->set(r, false) == 0)
        r->on = false;
}

/* --- Sets. --- */

/* The resources a message names, each once, sorted by kind (stably):
 * the count, or an error. */
static int64_t resolve(const struct PowerMsg *m, uint16_t *out, uint32_t *untouched)
{
    int idx[POWER_REFS];
    unsigned n = 0;

    *untouched = 0;
    if (m->pm_Name[0]) {
        const char *const *set = nullptr;

        for (unsigned b = 0; b < sizeof backends / sizeof backends[0] && !set; b++) {
            bool live = false;

            for (unsigned i = 0; i < pw_ndev; i++)
                live |= pw_dev[i].be == backends[b];
            for (const char *const *const *s = backends[b]->sets; live && s && *s; s++)
                if (streq((*s)[0], m->pm_Name))
                    set = *s + 1;
        }
        if (!set)
            return ERR_NOTFOUND;
        for (; *set && n < POWER_REFS; set++) {
            int i = pw_find(*set);

            if (i < 0)
                return ERR_NOTFOUND;    /* a backend's own table is wrong */
            idx[n++] = i;
        }
    } else {
        if (m->pm_Count == 0 || m->pm_Count > POWER_REFS)
            return ERR_BADARG;
        for (unsigned k = 0; k < m->pm_Count; k++) {
            const struct PowerRef *ref = &m->pm_Ref[k];
            int i;

            if (ref->pr_Device >= pw_ndev || !pw_dev[ref->pr_Device].be ||
                ref->pr_NCells > 4 || ref->pr_Kind < POWER_DOMAIN || ref->pr_Kind > POWER_RESET)
                return ERR_BADARG;
            if ((i = pw_dev[ref->pr_Device].be->xlate(ref->pr_Device, ref)) < 0) {
                (*untouched)++;
                continue;
            }
            if (pw_res[i].kind != ref->pr_Kind)
                return ERR_BADARG;
            idx[n++] = i;
        }
    }
    /* Each once, then by kind. */
    unsigned m2 = 0;

    for (unsigned k = 0; k < n; k++) {
        bool seen = false;

        for (unsigned j = 0; j < m2; j++)
            seen |= out[j] == idx[k];
        if (!seen)
            out[m2++] = (uint16_t)idx[k];
    }
    for (unsigned k = 1; k < m2; k++)
        for (unsigned j = k; j > 0 && pw_res[out[j - 1]].kind > pw_res[out[j]].kind; j--) {
            uint16_t t = out[j];

            out[j] = out[j - 1];
            out[j - 1] = t;
        }
    return m2;
}

static struct vote *find_vote(unsigned client, uint32_t tag)
{
    for (unsigned i = 0; i < PW_VOTES; i++)
        if (votes[i].used && votes[i].client == client && votes[i].tag == tag)
            return &votes[i];
    return nullptr;
}

static void release(struct vote *v)
{
    for (unsigned k = v->n; k-- > 0;)
        drop(v->res[k]);
    v->used = false;
}

static int64_t vote_on(unsigned client, struct PowerMsg *m)
{
    uint16_t res[POWER_REFS];
    struct vote *v = nullptr;
    uint32_t untouched;
    int64_t n, e = 0;
    unsigned k;

    if (find_vote(client, m->pm_Tag))
        return ERR_EXISTS;
    if ((n = resolve(m, res, &untouched)) < 0)
        return n;
    m->pm_Untouched = untouched;
    for (unsigned i = 0; i < PW_VOTES && !v; i++)
        if (!votes[i].used)
            v = &votes[i];
    if (!v)
        return ERR_FULL;
    m->pm_TurnedOn = 0;
    for (k = 0; k < n; k++) {
        bool turned;

        if ((e = take(res[k], &turned)) != 0)
            break;
        m->pm_TurnedOn += turned;
    }
    if (e) {
        while (k-- > 0)
            drop(res[k]);
        m->pm_TurnedOn = 0;
        return e;
    }
    *v = (struct vote){ .used = true, .n = (uint8_t)n, .client = (uint16_t)client, .tag = m->pm_Tag };
    for (k = 0; k < n; k++)
        v->res[k] = res[k];
    return 0;
}

static int64_t query(struct PowerMsg *m)
{
    uint16_t res[POWER_REFS];
    uint32_t untouched;
    int64_t n = resolve(m, res, &untouched);

    if (n < 0)
        return n;
    m->pm_Total = (uint32_t)n;
    m->pm_On = m->pm_Kept = 0;
    m->pm_Untouched = untouched;
    for (unsigned k = 0; k < n; k++) {
        struct pw_res *r = &pw_res[res[k]];
        int64_t on = r->be->get(r);

        if (on < 0)
            return on;
        m->pm_On += on == 1;
        m->pm_Kept += r->kept;
    }
    return 0;
}

void pw_message(unsigned client, struct PowerMsg *m)
{
    int64_t e;
    struct vote *v;

    m->pm_Name[POWER_NAME - 1] = '\0';
    switch (m->pm_Op) {
    case POWER_ON:
        e = vote_on(client, m);
        break;
    case POWER_OFF:
        if ((v = find_vote(client, m->pm_Tag))) {
            release(v);
            e = 0;
        } else {
            e = ERR_NOTFOUND;
        }
        break;
    case POWER_QUERY:
        e = query(m);
        break;
    default:
        e = ERR_BADARG;
    }
    m->pm_Msg.mn_Error = (int32_t)e;
}

void pw_gone(unsigned client)
{
    for (unsigned i = 0; i < PW_VOTES; i++)
        if (votes[i].used && votes[i].client == client)
            release(&votes[i]);
}

bool pw_poll32(volatile uint32_t *reg, uint32_t mask, uint32_t want, uint64_t us)
{
    uint64_t start = pw_now_us();

    for (;;) {
        if ((*reg & mask) == want)
            return true;
        if (pw_now_us() - start >= us)
            return (*reg & mask) == want;
        pw_delay_us(5);
    }
}
