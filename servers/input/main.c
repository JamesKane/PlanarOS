/*
 * servers/input/main.c -- input: keys and the mouse, from every driver
 * that has them, as events on one client's ring (v5-plan step 4)
 *
 * planar/devices/input.h has the protocol. hwdb starts input, before the
 * drivers, and again whenever it ends; a driver connects when "input" is
 * there, and again after it goes.
 *
 * A boot keyboard's report is its modifiers (a bit each), a reserved
 * byte, and the usages of up to six keys held (HID 1.11 appendix B.1).
 * A key is down from the first report it's in to the first it isn't;
 * the modifiers come as keys too (0xe0 to 0xe7) and as each event's
 * qualifier. A boot mouse's is its buttons and a signed byte each of x
 * and y (B.2). Six keys and an error code in every slot ("phantom")
 * means more were held than it can say: that report is ignored.
 *
 * Caps Lock is input's own: the keyboard's light isn't set.
 */
#include <stddef.h>

#include <planar/devices/input.h>
#include <planar/syscalls.h>

void *memset(void *dst, int c, size_t n);
void *memcpy(void *restrict dst, const void *restrict src, size_t n);

constexpr int64_t SIG_ACCEPT = 16;
constexpr int64_t SIG_CONN = 17;
constexpr int64_t SIG_TIMER = 18;
constexpr unsigned CONNS = 16;
constexpr unsigned DEVICES = 16;         /* attached, every driver's */
constexpr uint64_t MS = 1000000;
constexpr uint64_t REPEAT_DELAY = 500 * MS;
constexpr uint64_t REPEAT_EVERY = 33 * MS;

static struct {
    Handle port;
    struct MsgPort conn[CONNS];
    unsigned nconns;
    struct InputRing *ring;
    Handle ring_area;
    int holder;                         /* the connection with the ring, or -1 */
    Handle holder_task;
    uint64_t events;                    /* written, ever */
    uint16_t caps;                      /* IEQ_CAPSLOCK, or 0 */
    struct {                            /* the key repeating */
        uint16_t code, qual;
        uint32_t ch;
        uint64_t at;                    /* when it next repeats; 0 if none */
        int dev;
    } rep;
} in = { .holder = -1, .rep = { .dev = -1 } };

static struct device {
    bool used;
    unsigned conn;                      /* its driver's */
    uint32_t id;                        /* the driver's number for it */
    uint32_t kind;
    uint8_t last[8];                    /* the last report taken */
} dev[DEVICES];

static void say(const char *s)
{
    KLog(s);
}

/* --- The ring. --- */

static void post(uint16_t cls, uint16_t code, uint16_t flags, uint16_t qual, uint32_t ch, int32_t x,
                 int32_t y)
{
    struct InputEvent *e;

    if (in.holder < 0)
        return;
    e = &in.ring->ir_Event[in.ring->ir_Head % INPUT_RING];
    *e = (struct InputEvent){ .ie_Time = CurrentTime(), .ie_Class = cls, .ie_Code = code,
                              .ie_Qualifier = qual, .ie_Flags = flags, .ie_Char = ch,
                              .ie_X = x, .ie_Y = y };
    __atomic_store_n(&in.ring->ir_Head, in.ring->ir_Head + 1, __ATOMIC_RELEASE);
    in.events++;
}

static void wake(void)
{
    WakeAddr(&in.ring->ir_Head, UINT32_MAX);
}

/* --- Keys. --- */

/* What usage 'k' types on a US layout, with these qualifiers; 0 if nothing. */
static uint32_t typed(uint16_t k, uint16_t q)
{
    static const char plain[] = "\n\033\b\t -=[]\\#;'`,./";   /* 0x28 to 0x38 */
    static const char shifted[] = "\n\033\b\t _+{}|~:\"~<>?";
    static const char digits[] = "1234567890", above[] = "!@#$%^&*()";
    bool shift = (q & IEQ_SHIFT) != 0;

    if (k >= IEKEY_A && k < IEKEY_A + 26) {
        char c = (char)('a' + (k - IEKEY_A));

        if (q & IEQ_CTRL)
            return (uint32_t)(c & 0x1f);
        if (shift != ((q & IEQ_CAPSLOCK) != 0))
            c = (char)(c - 32);
        return (uint32_t)c;
    }
    if (k >= 0x1e && k <= 0x27)
        return (uint32_t)(shift ? above : digits)[k - 0x1e];
    if (k >= IEKEY_ENTER && k <= 0x38)
        return (uint32_t)(shift ? shifted : plain)[k - IEKEY_ENTER];
    if (k == IEKEY_DELETE)
        return 127;
    if (k == 0x58)                      /* the keypad's Enter */
        return '\n';
    return 0;
}

static bool held(const uint8_t *r, uint8_t k)
{
    for (unsigned i = 2; i < 8; i++)
        if (r[i] == k)
            return true;
    return false;
}

static void key(int d, uint16_t code, bool down, uint16_t qual)
{
    uint32_t ch = down ? typed(code, qual) : 0;

    if (code == IEKEY_CAPSLOCK && down)
        in.caps ^= IEQ_CAPSLOCK;
    post(IECLASS_KEY, code, down ? 0 : IEF_UP, qual, ch, 0, 0);
    if (down && ch) {
        in.rep = (typeof(in.rep)){ .code = code, .qual = qual, .ch = ch,
                                   .at = CurrentTime() + REPEAT_DELAY, .dev = d };
    } else if ((!down && in.rep.code == code && in.rep.dev == d) || (down && code < IEKEY_LCTRL)) {
        in.rep.at = 0;                  /* let go, or another key: no repeat */
    }
}

static void keyboard(int d, const uint8_t *r)
{
    struct device *v = &dev[d];
    uint16_t was = v->last[0] | in.caps, now = r[0] | in.caps;

    if (r[2] == 1 && r[7] == 1)         /* ErrorRollOver: too many keys */
        return;
    for (unsigned b = 0; b < 8; b++)
        if ((was ^ now) & 1u << b)
            key(d, (uint16_t)(IEKEY_LCTRL + b), now & 1u << b, now);
    for (unsigned i = 2; i < 8; i++)
        if (v->last[i] > 3 && !held(r, v->last[i]))
            key(d, v->last[i], false, now);
    for (unsigned i = 2; i < 8; i++)
        if (r[i] > 3 && !held(v->last, r[i])) {
            key(d, r[i], true, now);
            now = (uint16_t)(r[0] | in.caps);
        }
    memcpy(v->last, r, 8);
}

static void mouse(int d, const uint8_t *r, uint32_t len)
{
    struct device *v = &dev[d];

    for (unsigned b = 0; b < 3; b++)
        if ((v->last[0] ^ r[0]) & 1u << b)
            post(IECLASS_BUTTON, (uint16_t)b, r[0] & 1u << b ? 0 : IEF_UP, in.caps, 0, 0, 0);
    if (len >= 3 && (r[1] || r[2]))
        post(IECLASS_MOVE, 0, 0, in.caps, 0, (int8_t)r[1], (int8_t)r[2]);
    v->last[0] = r[0];
}

/* Everything 'd' held, let go: it went. */
static void release(int d)
{
    static const uint8_t none[8];

    if (dev[d].kind == INPUT_KEYBOARD)
        keyboard(d, none);
    else
        mouse(d, none, 1);
    if (in.rep.dev == d)
        in.rep.at = 0;
    dev[d].used = false;
}

/* --- Messages. --- */

static int find(unsigned c, uint32_t id)
{
    for (int d = 0; d < (int)DEVICES; d++)
        if (dev[d].used && dev[d].conn == c && dev[d].id == id)
            return d;
    return -1;
}

static int64_t take_ring(unsigned c, struct InputMsg *m)
{
    int64_t e;

    if (in.holder >= 0)
        return ERR_BUSY;
    if ((e = ShareArea(in.ring_area, m->im_Task, AREAR_READ)) != 0)
        return e;
    in.holder = (int)c;
    in.holder_task = m->im_Task;
    m->im_Ring = (uint64_t)in.ring;
    return 0;
}

static void message(unsigned c, struct InputMsg *m)
{
    struct InputMsg q;
    int d;

    memcpy(&q, m, sizeof q);
    m->im_Msg.mn_Error = 0;
    switch (q.im_Op) {
    case INPUT_OPEN:
        m->im_Msg.mn_Error = (int32_t)take_ring(c, m);
        break;
    case INPUT_INFO:
        m->im_Keyboards = m->im_Mice = 0;
        for (unsigned k = 0; k < DEVICES; k++) {
            m->im_Keyboards += dev[k].used && dev[k].kind == INPUT_KEYBOARD;
            m->im_Mice += dev[k].used && dev[k].kind == INPUT_MOUSE;
        }
        m->im_Held = in.holder >= 0;
        m->im_Events = in.events;
        break;
    case INPUT_ATTACH:
        if ((q.im_Kind != INPUT_KEYBOARD && q.im_Kind != INPUT_MOUSE) ||
            find(c, q.im_Device) >= 0) {
            m->im_Msg.mn_Error = (int32_t)ERR_BADARG;
            break;
        }
        for (d = 0; d < (int)DEVICES && dev[d].used; d++)
            ;
        if (d == (int)DEVICES) {
            m->im_Msg.mn_Error = (int32_t)ERR_FULL;
            break;
        }
        dev[d] = (struct device){ .used = true, .conn = c, .id = q.im_Device, .kind = q.im_Kind };
        say(q.im_Kind == INPUT_KEYBOARD ? "input: a keyboard" : "input: a mouse");
        break;
    case INPUT_DETACH:
        if ((d = find(c, q.im_Device)) < 0) {
            m->im_Msg.mn_Error = (int32_t)ERR_NOTFOUND;
            break;
        }
        release(d);
        break;
    case INPUT_REPORT:
        if ((d = find(c, q.im_Device)) < 0 || q.im_Len > INPUT_REPORT_MAX) {
            m->im_Msg.mn_Error = (int32_t)ERR_BADARG;
            break;
        }
        if (dev[d].kind == INPUT_KEYBOARD && q.im_Len >= 8)
            keyboard(d, q.im_Data);
        else if (dev[d].kind == INPUT_MOUSE && q.im_Len >= 1)
            mouse(d, q.im_Data, q.im_Len);
        break;
    default:
        m->im_Msg.mn_Error = (int32_t)ERR_BADARG;
    }
    ReplyMsgLater(&in.conn[c], &m->im_Msg);
}

/* Connection c went: what it held goes, and the last takes its place. */
static void gone(unsigned c)
{
    unsigned last = in.nconns - 1;

    for (int d = 0; d < (int)DEVICES; d++)
        if (dev[d].used && dev[d].conn == c)
            release(d);
    if (in.holder == (int)c) {
        UnshareArea(in.ring_area, in.holder_task);
        in.holder = -1;
    }
    ClosePort(&in.conn[c]);
    if (c != last) {
        in.conn[c] = in.conn[last];
        for (int d = 0; d < (int)DEVICES; d++)
            if (dev[d].used && dev[d].conn == last)
                dev[d].conn = c;
        if (in.holder == (int)last)
            in.holder = (int)c;
    }
    in.nconns--;
}

int main(uint64_t)
{
    int64_t h;
    void *at;

    if ((h = AllocArea(sizeof *in.ring, AREAF_CLEAR, &at)) < 0) {
        say("input: no memory for the ring");
        return 1;
    }
    in.ring_area = (Handle)h;
    in.ring = at;
    if ((h = CreatePort("input", SIG_ACCEPT)) < 0) {
        say("input: \"input\" couldn't be published");
        return 1;
    }
    in.port = (Handle)h;
    for (;;) {
        uint64_t before = in.events;
        uint64_t now;

        if (in.rep.at) {
            ArmTimer(in.rep.at, SIG_TIMER);
            WaitPorts(in.conn, in.nconns,
                      1ULL << SIG_ACCEPT | 1ULL << SIG_CONN | 1ULL << SIG_TIMER);
            ArmTimer(0, 0);
        } else {
            WaitPorts(in.conn, in.nconns, 1ULL << SIG_ACCEPT | 1ULL << SIG_CONN);
        }
        while (in.nconns < CONNS && AcceptPort(&in.conn[in.nconns], in.port, SIG_CONN) == 0)
            in.nconns++;
        for (unsigned c = 0; c < in.nconns;) {
            struct InputMsg *m;

            while ((m = (struct InputMsg *)GetMsgSized(&in.conn[c], sizeof *m)))
                message(c, m);
            if (PortGone(&in.conn[c])) {
                gone(c);
                continue;
            }
            c++;
        }
        now = CurrentTime();
        if (in.rep.at && now >= in.rep.at) {
            post(IECLASS_KEY, in.rep.code, IEF_REPEAT, in.rep.qual, in.rep.ch, 0, 0);
            in.rep.at = now + REPEAT_EVERY;
        }
        if (in.events != before)
            wake();
    }
}
