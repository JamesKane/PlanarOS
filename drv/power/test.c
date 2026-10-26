/*
 * drv/power/test.c -- the power server's core and backends on the host
 * (make power-test)
 *
 * The core against the model: votes, their order, counts and tags, a
 * vote that fails half way, a client's going, the firmware's resources
 * kept. The K1's backend against a made-up APMU whose status follows its
 * control bits. The SCMI backend against a made-up agent on both of the
 * CIX's channels, which checks each message as the SCP and TF-A would,
 * and the words of mailbox 6's page that are others'.
 */
#include <stdio.h>
#include <string.h>

#include <planar/syscalls.h>

#include "power.h"
#include "smt.h"

static unsigned checks, failures;

#define CHECK(cond)                                                                 \
    do {                                                                            \
        checks++;                                                                   \
        if (!(cond)) {                                                              \
            failures++;                                                             \
            fprintf(stderr, "%s:%d: failed: %s\n", __FILE__, __LINE__, #cond);      \
        }                                                                           \
    } while (0)

/* --- What main.c gives the server, here made up. --- */

static uint64_t now_us;
static void (*hardware)(void);      /* the made-up devices' next move */

void pw_delay_us(uint64_t us)
{
    now_us += us;
    if (hardware)
        hardware();
}

uint64_t pw_now_us(void)
{
    return now_us;
}

void pw_say(const char *text)
{
    printf("  %s\n", text);
}

static int64_t (*firmware)(uint64_t x[4]);
static uint8_t *mapped;
static uint64_t mapped_base;

int64_t pw_firmware_call(uint64_t x[4])
{
    return firmware ? firmware(x) : ERR_RIGHTS;
}

volatile uint8_t *pw_map(uint64_t base, uint64_t size)
{
    return mapped && base == mapped_base && size <= 0x80 ? mapped : nullptr;
}

/* --- Messages. --- */

static struct PowerMsg msg;

static int64_t ask(unsigned client, uint32_t op, uint32_t tag, const char *name)
{
    memset(&msg, 0, sizeof msg);
    msg.pm_Op = op;
    msg.pm_Tag = tag;
    if (name)
        snprintf(msg.pm_Name, sizeof msg.pm_Name, "%s", name);
    pw_message(client, &msg);
    return msg.pm_Msg.mn_Error;
}

static int64_t ask_refs(unsigned client, uint32_t op, uint32_t tag, unsigned n,
                        const struct PowerRef *refs)
{
    memset(&msg, 0, sizeof msg);
    msg.pm_Op = op;
    msg.pm_Tag = tag;
    msg.pm_Count = n;
    memcpy(msg.pm_Ref, refs, n * sizeof refs[0]);
    pw_message(client, &msg);
    return msg.pm_Msg.mn_Error;
}

static bool on(const char *name)
{
    int i = pw_find(name);

    return i >= 0 && pw_res[i].be->get(&pw_res[i]) == 1;
}

static struct PowerRef ref(uint32_t kind, uint32_t cell)
{
    return (struct PowerRef){ .pr_Kind = kind, .pr_Device = 0, .pr_NCells = 1, .pr_Cells = { cell } };
}

/* --- The core, on the model. --- */

static void test_core(void)
{
    struct PowerRef r[POWER_REFS + 1];

    pw_reset();
    pw_model_ntrace = 0;
    pw_device("planar,power-model", nullptr, 0, 0);
    pw_device("vendor,unknown", nullptr, 0, 0);
    CHECK(pw_dev[0].be == &pw_model && !pw_dev[1].be);
    CHECK(pw_start());
    CHECK(pw_nres == 7 && pw_res[0].kept && !pw_res[1].kept);

    /* Order: domains, clocks, resets, whatever order they're named in. */
    r[0] = ref(POWER_RESET, 0);
    r[1] = ref(POWER_CLOCK, 1);
    r[2] = ref(POWER_DOMAIN, 2);
    r[3] = ref(POWER_CLOCK, 1);             /* twice: once */
    r[4] = ref(POWER_CLOCK, 7);             /* no such clock: left alone */
    CHECK(ask_refs(0, POWER_ON, 5, 5, r) == 0 && msg.pm_TurnedOn == 3 && msg.pm_Untouched == 1);
    CHECK(pw_model_ntrace == 3 && pw_model_trace[0] == (0x80 | 2) && pw_model_trace[1] == (0x80 | 5) &&
          pw_model_trace[2] == (0x80 | 6));
    CHECK(ask_refs(0, POWER_ON, 5, 1, r) == ERR_EXISTS);
    /* Another client shares one: it stays on until both have gone. */
    CHECK(ask_refs(1, POWER_ON, 5, 1, &r[1]) == 0 && msg.pm_TurnedOn == 0);
    CHECK(pw_votes() == 2);
    pw_model_ntrace = 0;
    pw_gone(0);
    CHECK(pw_votes() == 1 && on("model.clock1") && !on("model.domain2") && !on("model.reset0"));
    CHECK(pw_model_ntrace == 2 && pw_model_trace[0] == 6 && pw_model_trace[1] == 2);  /* reverse */
    CHECK(ask(1, POWER_OFF, 5, nullptr) == 0 && !on("model.clock1") && pw_votes() == 0);
    CHECK(ask(1, POWER_OFF, 5, nullptr) == ERR_NOTFOUND);

    /* The firmware's: counted, never turned off. */
    r[0] = ref(POWER_DOMAIN, 0);
    CHECK(ask_refs(2, POWER_ON, 1, 1, r) == 0 && msg.pm_TurnedOn == 0);
    pw_gone(2);
    CHECK(on("model.domain0"));

    /* Half way: domain 1 comes on, domain 3 never does; both end off. */
    CHECK(ask(3, POWER_ON, 1, "broken") == ERR_TIMEOUT && !on("model.domain1") && pw_votes() == 0);
    CHECK(pw_res[pw_find("model.domain1")].votes == 0);

    /* Queries, by name and by references. */
    CHECK(ask(3, POWER_QUERY, 0, "gpu") == 0 && msg.pm_Total == 3 && msg.pm_On == 0);
    CHECK(ask(3, POWER_ON, 9, "gpu") == 0 && msg.pm_TurnedOn == 3);
    CHECK(ask(3, POWER_QUERY, 0, "gpu") == 0 && msg.pm_On == 3 && msg.pm_Kept == 0);
    r[0] = ref(POWER_DOMAIN, 0);
    r[1] = ref(POWER_DOMAIN, 2);
    CHECK(ask_refs(4, POWER_QUERY, 0, 2, r) == 0 && msg.pm_Total == 2 && msg.pm_On == 2 &&
          msg.pm_Kept == 1);
    pw_gone(3);

    /* Refused. */
    CHECK(ask(4, POWER_ON, 1, "no-such") == ERR_NOTFOUND);
    CHECK(ask(4, POWER_ON, 1, nullptr) == ERR_BADARG);
    CHECK(ask(4, 77, 1, nullptr) == ERR_BADARG);
    r[0] = (struct PowerRef){ .pr_Kind = POWER_CLOCK, .pr_Device = 1, .pr_NCells = 1 };  /* no backend */
    CHECK(ask_refs(4, POWER_ON, 1, 1, r) == ERR_BADARG);
    r[0] = (struct PowerRef){ .pr_Kind = 9, .pr_Device = 0, .pr_NCells = 1 };
    CHECK(ask_refs(4, POWER_ON, 1, 1, r) == ERR_BADARG);
    r[0] = (struct PowerRef){ .pr_Kind = POWER_CLOCK, .pr_Device = 0, .pr_NCells = 5 };
    CHECK(ask_refs(4, POWER_ON, 1, 1, r) == ERR_BADARG);
    for (unsigned i = 0; i <= POWER_REFS; i++)
        r[i] = ref(POWER_CLOCK, 0);
    memset(&msg, 0, sizeof msg);
    msg.pm_Op = POWER_ON;
    msg.pm_Count = POWER_REFS + 1;
    pw_message(4, &msg);
    CHECK(msg.pm_Msg.mn_Error == ERR_BADARG);
    /* A name not ended: cut, not read past. */
    memset(&msg, 0, sizeof msg);
    msg.pm_Op = POWER_QUERY;
    memset(msg.pm_Name, 'g', sizeof msg.pm_Name);
    pw_message(4, &msg);
    CHECK(msg.pm_Msg.mn_Error == ERR_NOTFOUND);

    /* Full: every vote slot taken. */
    unsigned took = 0;

    for (uint32_t t = 0; t < PW_VOTES + 1; t++)
        took += ask(5, POWER_ON, t, "gpu") == 0;
    CHECK(took == PW_VOTES && ask(5, POWER_ON, 999, "gpu") == ERR_FULL);
    pw_gone(5);
    CHECK(pw_votes() == 0 && !on("model.domain2"));
}

/* --- The K1: a made-up APMU. --- */

static uint32_t apmu[0x400 / 4];
static bool k1_stuck;               /* the VPU's domain never answers */

/* Each domain's status follows its control register: the sleep switches
 * and isolation for the software ones, auto and hardware mode for the
 * others (k1.c). */
static void k1_hardware(void)
{
    static const struct { uint32_t ctrl, bit; bool hw; } d[] = {
        { 0x0a8, 1, false }, { 0x0d0, 0, false }, { 0x380, 12, true }, { 0x37c, 2, false },
        { 0x378, 11, true }, { 0x13c, 6, false }, { 0x3f4, 15, true },
    };

    for (unsigned i = 0; i < sizeof d / sizeof d[0]; i++) {
        uint32_t c = apmu[d[i].ctrl / 4];
        bool up = d[i].hw ? (c & 0x11) == 0x11 : (c & 0xe) == 0xe;

        if (k1_stuck && i == 0)
            continue;
        apmu[0xf0 / 4] = (apmu[0xf0 / 4] & ~(1u << d[i].bit)) | (up ? 1u << d[i].bit : 0);
    }
    /* The GPU clock's frequency change: done only with its gate open and
     * the GPU's reset released, as on the board. */
    if ((apmu[0xcc / 4] & 0x12) == 0x12)
        apmu[0xcc / 4] &= ~(1u << 15);
}

static void test_k1(void)
{
    struct PowerRef r[4];

    pw_reset();
    memset(apmu, 0, sizeof apmu);
    /* As U-Boot leaves it: HDMI's domain on, SD 0's clocks and resets
     * on, and the GPU's register with a mux and divider set. */
    apmu[0x3f4 / 4] = 0x11;
    apmu[0x054 / 4] = 0x1b | 0x100;
    apmu[0x0cc / 4] = 0x00145000;
    hardware = k1_hardware;
    k1_hardware();
    pw_device("ky,x1-ccu", (volatile uint8_t *)apmu, 0xd4282800, 0x400);
    pw_device("ky,x1-reset", (volatile uint8_t *)apmu, 0xd4282800, 0x400);
    pw_device("ky,x1-pm-domain", (volatile uint8_t *)apmu, 0xd4282800, 0x400);
    CHECK(pw_start());
    CHECK(pw_res[pw_find("k1.pd.hdmi")].kept && !pw_res[pw_find("k1.pd.gpu")].kept);
    CHECK(pw_res[pw_find("k1.clk.sdh0")].kept && pw_res[pw_find("k1.rst.sdh0")].kept);

    /* The GPU: domain, clock, reset, then the clock's rate. */
    CHECK(ask(0, POWER_QUERY, 0, "gpu") == 0 && msg.pm_Total == 3 && msg.pm_On == 0);
    CHECK(ask(0, POWER_ON, 1, "gpu") == 0 && msg.pm_TurnedOn == 3);
    CHECK((apmu[0xd0 / 4] & 0xe) == 0xe && (apmu[0xf0 / 4] & 1));
    CHECK(apmu[0xcc / 4] == 0x12);                  /* 614.4 MHz: mux 0, divided by 1 */
    pw_gone(0);
    CHECK((apmu[0xd0 / 4] & 0xe) == 0 && !(apmu[0xf0 / 4] & 1) && apmu[0xcc / 4] == 0);

    /* SD 0 by its references (U-Boot's cells): the firmware's, so left on. */
    r[0] = (struct PowerRef){ .pr_Kind = POWER_CLOCK, .pr_Device = 0, .pr_NCells = 1, .pr_Cells = { 0xb2 } };
    r[1] = (struct PowerRef){ .pr_Kind = POWER_CLOCK, .pr_Device = 0, .pr_NCells = 1, .pr_Cells = { 0xb1 } };
    r[2] = (struct PowerRef){ .pr_Kind = POWER_RESET, .pr_Device = 1, .pr_NCells = 1, .pr_Cells = { 0x48 } };
    r[3] = (struct PowerRef){ .pr_Kind = POWER_CLOCK, .pr_Device = 0, .pr_NCells = 1, .pr_Cells = { 0x63 } };
    CHECK(ask_refs(1, POWER_ON, 1, 4, r) == 0 && msg.pm_TurnedOn == 0 && msg.pm_Untouched == 1);
    pw_gone(1);
    CHECK(apmu[0x54 / 4] == (0x1b | 0x100));
    /* A clock cell on the reset controller is the wrong kind of thing. */
    r[0].pr_Device = 1;
    CHECK(ask_refs(1, POWER_ON, 2, 1, r) == 0 && msg.pm_Untouched == 1);
    pw_gone(1);

    /* HDMI's domain by its cell (6): a hardware-mode one, kept on. */
    r[0] = (struct PowerRef){ .pr_Kind = POWER_DOMAIN, .pr_Device = 2, .pr_NCells = 1, .pr_Cells = { 6 } };
    CHECK(ask_refs(2, POWER_ON, 1, 1, r) == 0 && msg.pm_TurnedOn == 0);
    pw_gone(2);
    CHECK(apmu[0x3f4 / 4] == 0x11);
    /* The LCD's (2), off: switched by the hardware bits. */
    r[0].pr_Cells[0] = 2;
    CHECK(ask_refs(2, POWER_ON, 1, 1, r) == 0 && msg.pm_TurnedOn == 1 && (apmu[0x380 / 4] & 0x11) == 0x11);
    pw_gone(2);
    CHECK((apmu[0x380 / 4] & 0x11) == 0);

    /* A domain that never says it's on: a timeout, after the vendor's 60 ms. */
    k1_stuck = true;
    r[0].pr_Cells[0] = 0;
    uint64_t t0 = now_us;
    CHECK(ask_refs(3, POWER_ON, 1, 1, r) == ERR_TIMEOUT && now_us - t0 >= 60000 && pw_votes() == 0);
    k1_stuck = false;
    hardware = nullptr;
}

/* --- The CIX: a made-up SCP on mailbox 6, and TF-A on the SMC channel. --- */

static uint32_t mbx[0x10000 / 4];   /* mailbox 6's page, SHM0 at its start */
static uint32_t smc_shm[0x80 / 4];
static bool domains_on[22];
static bool clock_on[0x200];
static unsigned agent_bad, mbx_calls, smc_calls, deny_domain = 99;
static uint32_t last_token[2];

enum { STATUS = 1, FLAGS = 4, LENGTH = 5, HEADER = 6, PAYLOAD = 7 };

/* Answer the message in a channel as an SCMI agent: what it checks of
 * the message, then the answer, then the channel free. */
static void agent(uint32_t *ch, bool is_smc)
{
    uint32_t h = ch[HEADER], msgid = h & 0xff, proto = h >> 10 & 0xff, token = h >> 18 & 0x3ff;
    uint32_t len = ch[LENGTH], *p = &ch[PAYLOAD];
    int32_t st = 0;
    unsigned out = 1;

    if (ch[STATUS] & 1 || ch[FLAGS] != 0 || (h >> 8 & 3) != 0 || len < 4 ||
        token == last_token[is_smc])
        agent_bad++;
    last_token[is_smc] = token;
    if (msgid == 0) {
        p[1] = 0x20000;
        out = 2;
    } else if (proto == 0x11 && is_smc && msgid == 4 && len == 16) {
        if (p[1] >= 22 || p[0] != 0)
            st = -2;
        else if (p[1] == deny_domain)
            st = -3;
        else if (p[2] == 0 || p[2] == 0x40000000)
            domains_on[p[1]] = p[2] == 0;
        else
            st = -2;
    } else if (proto == 0x11 && is_smc && msgid == 5 && len == 8) {
        if (p[0] >= 22) {
            st = -4;
        } else {
            p[1] = domains_on[p[0]] ? 0 : 0x40000000;
            out = 2;
        }
    } else if (proto == 0x14 && !is_smc && msgid == 3 && len == 8 && p[0] < 0x200) {
        p[1] = clock_on[p[0]];
        memcpy(&p[2], "gpu_clk\0\0\0\0\0\0\0\0\0", 16);
        out = 6;
    } else if (proto == 0x14 && !is_smc && msgid == 7 && len == 12 && p[0] < 0x200) {
        clock_on[p[0]] = p[1] & 1;
    } else {
        agent_bad++;
        st = -1;
    }
    p[0] = (uint32_t)st;
    ch[LENGTH] = 4 + 4 * out;
    ch[STATUS] = 1;
}

/* The doorbell, at +0x80: rung, it's answered at the next tick. */
static void scp(void)
{
    if (mbx[0x80 / 4] == 1) {
        mbx[0x80 / 4] = 0;
        mbx_calls++;
        agent(mbx, false);
    }
}

static int64_t tfa(uint64_t x[4])
{
    if (x[0] != 0xc2000001)
        return ERR_RIGHTS;
    smc_calls++;
    agent(smc_shm, true);
    x[0] = 0;
    return 0;
}

/* hwdb's clock for smt_release: each look a microsecond on. */
static uint64_t tick(void)
{
    pw_delay_us(1);
    return now_us;
}

static void test_scmi(void)
{
    pw_reset();
    memset(mbx, 0, sizeof mbx);
    memset(smc_shm, 0, sizeof smc_shm);
    mbx[STATUS] = smc_shm[STATUS] = 1;
    for (unsigned i = 0x84 / 4; i < 0xd0 / 4; i++)
        mbx[i] = 0xc0de0000 | i;            /* CPPC's and PCC's: not to be touched */
    domains_on[3] = domains_on[4] = true;  /* the hubs, left on */
    clock_on[0x110] = true;
    hardware = scp;
    firmware = tfa;
    mapped = (uint8_t *)smc_shm;
    mapped_base = 0x84380000;
    pw_device("CIXHA001", (volatile uint8_t *)mbx, 0x6590000, 0x10000);
    pw_device("CIXHA004", (volatile uint8_t *)mbx, 0x6590000, 0x80);
    CHECK(pw_start());
    CHECK(pw_find("cix.pd.gpu") >= 0 && pw_find("cix.clk.gpu-core") >= 0);
    CHECK(pw_res[pw_find("cix.pd.pciehub")].kept && pw_res[pw_find("cix.clk.gpu-200m")].kept);
    CHECK(!pw_res[pw_find("cix.pd.gpu")].kept);

    unsigned m0 = mbx_calls, s0 = smc_calls;

    CHECK(ask(0, POWER_ON, 1, "gpu") == 0 && msg.pm_TurnedOn == 3);
    CHECK(domains_on[21] && clock_on[0x1f] && clock_on[0x20]);
    CHECK(mbx_calls == m0 + 2 && smc_calls == s0 + 1);
    CHECK(ask(0, POWER_QUERY, 0, "gpu") == 0 && msg.pm_On == 3);
    pw_gone(0);
    CHECK(!domains_on[21] && !clock_on[0x1f] && !clock_on[0x20] && clock_on[0x110]);

    /* Refused by the far side: the kernel's error, and nothing left on. */
    deny_domain = 21;
    CHECK(ask(1, POWER_ON, 1, "gpu") == ERR_RIGHTS && pw_votes() == 0);
    deny_domain = 99;
    /* An SCP that never answers: busy, and a timeout. */
    hardware = nullptr;
    uint64_t t0 = now_us;
    CHECK(ask(2, POWER_QUERY, 0, "gpu") == ERR_TIMEOUT && now_us - t0 >= 300000);
    CHECK(ask(2, POWER_QUERY, 0, "gpu") == ERR_BUSY);      /* the channel is still its */
    mbx[STATUS] = 1;
    mbx[0x80 / 4] = 0;                  /* its ring, never taken */
    hardware = scp;

    /* hwdb's stop method on the channels a dead power left (smt.h). Free:
     * left alone, at once. */
    t0 = now_us;
    CHECK(!smt_release((volatile uint8_t *)mbx, false, tick, now_us + 300000) && now_us == t0);
    CHECK(!smt_release((volatile uint8_t *)smc_shm, true, tick, now_us + 300000));
    /* Killed between busy and the ring: the SMC's freed at once, the
     * mailbox's after the wait, and each answers the next power. */
    smc_shm[STATUS] = 0;
    CHECK(smt_release((volatile uint8_t *)smc_shm, true, tick, now_us + 300000) && now_us == t0);
    CHECK(smc_shm[STATUS] == 1);
    mbx[STATUS] = 0;
    CHECK(smt_release((volatile uint8_t *)mbx, false, tick, now_us + 300000));
    CHECK(now_us - t0 >= 300000 && mbx[STATUS] == 1);
    /* Killed after the ring: the SCP's answer waited for, and kept. */
    unsigned m1 = mbx_calls;

    mbx[HEADER] = 0x14 << 10 | ((last_token[0] + 0x200) & 0x3ff) << 18;   /* clocks' version */
    mbx[LENGTH] = 4;
    mbx[STATUS] = 0;
    mbx[0x80 / 4] = 1;
    t0 = now_us;
    CHECK(!smt_release((volatile uint8_t *)mbx, false, tick, now_us + 300000));
    CHECK(mbx_calls == m1 + 1 && now_us - t0 < 1000 && mbx[PAYLOAD + 1] == 0x20000);
    CHECK(ask(2, POWER_QUERY, 0, "gpu") == 0);

    CHECK(agent_bad == 0);
    bool untouched = true;          /* the doorbell (0x80) is the server's; above it, others' */

    for (unsigned i = 0x84 / 4; i < 0xd0 / 4; i++)
        untouched &= mbx[i] == (0xc0de0000 | i);
    CHECK(untouched);

    /* No SMC channel in the lease: clocks alone. */
    pw_reset();
    mapped = nullptr;
    pw_device("CIXHA001", (volatile uint8_t *)mbx, 0x6590000, 0x10000);
    pw_device("CIXHA004", (volatile uint8_t *)mbx, 0x6590000, 0x80);
    CHECK(pw_start() && pw_find("cix.pd.gpu") < 0 && pw_find("cix.clk.gpu-core") >= 0);
    CHECK(ask(3, POWER_ON, 1, "gpu") == ERR_NOTFOUND);
    hardware = nullptr;
    firmware = nullptr;
}

int main(void)
{
    test_core();
    test_k1();
    test_scmi();
    printf("power-test: %u of %u checks failed\n", failures, checks);
    return failures != 0;
}
