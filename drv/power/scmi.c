/*
 * drv/power/scmi.c -- the CIX P1's power, by SCMI (v5-plan step 2)
 *
 * Two channels, each a shared-memory mailbox in the SMT layout (SCMI
 * 3.2 §5.1; CIX's Linux and FreeBSD's sys/arm64/cix/sky1_scmi.c):
 *
 * - Clocks (protocol 0x14) go to the system control processor: the
 *   channel is the first 0x80 bytes of mailbox 6 (ACPI's SHM0, at
 *   0x6590000), rung by writing 1 at +0x80 (MBX6's doorbell) and polled
 *   until the channel is free again. The rest of that page is others':
 *   the CPUs' CPPC registers at 0x8c-0xa8, the PCC doorbell at 0x90, the
 *   interrupt registers. Nothing here touches them.
 * - Power domains (protocol 0x11) are TF-A's: the channel is at
 *   0x84380000, which only the device tree names (a kernel quirk lets it
 *   be leased), and it's rung by SMC 0xc2000001, which the kernel makes
 *   for the lease that hwdb allowed it (CallLease). The answer is there
 *   when the call returns.
 *
 * Every access to a channel is 32 bits wide, as CIX's own code makes
 * them. Domain IDs are the vendor tree's (dt-bindings/power/
 * sky1-power.h), clock IDs too (sky1-clk.h); ACPI names neither, so
 * nothing here is reached by a firmware reference, only by the named
 * set "gpu": domain 21 and the clocks gpu_clk_core and gpu_clk_stacks,
 * the first two of the G720's five steps. The other three (the power
 * resource GPUP.PPRS, the reset pulse, the Q-channel) are in the GPU's
 * own registers, and step 5's.
 */
#include <planar/syscalls.h>
#include <planar/utility.h>

#include "power.h"
#include "smt.h"

constexpr uint64_t SMC_SHMEM = 0x84380000;
constexpr uint32_t SMC_FID = 0xc2000001;
constexpr uint32_t DB = 0x80;               /* MBX6's doorbell, from its base */

constexpr unsigned SMT_WORDS = (SMT_SIZE - SMT_PAYLOAD) / 4;

enum : uint32_t { P_BASE = 0x10, P_POWER = 0x11, P_CLOCK = 0x14 };
enum : uint32_t { M_VERSION = 0, PD_STATE_SET = 4, PD_STATE_GET = 5, CLK_ATTRIBUTES = 3,
                  CLK_CONFIG_SET = 7 };
constexpr uint32_t PD_ON = 0, PD_OFF = 0x40000000;
constexpr uint64_t BUSY_US = 300000;

struct chan {
    volatile uint8_t *shm;
    volatile uint32_t *doorbell;    /* nullptr: rung by SMC */
    uint16_t token;
};

static struct chan mbox, smc;

static const char *const domain_names[] = {
    "cix.pd.audio", "cix.pd.pcie-ctrl0", "cix.pd.pcie-dummy", "cix.pd.pciehub", "cix.pd.mmhub",
    "cix.pd.mmhub-smmu", "cix.pd.dpu0", "cix.pd.dpu1", "cix.pd.dpu2", "cix.pd.dpu3",
    "cix.pd.dpu4", "cix.pd.vpu-top", "cix.pd.vpu-core0", "cix.pd.vpu-core1", "cix.pd.vpu-core2",
    "cix.pd.vpu-core3", "cix.pd.npu-core0", "cix.pd.npu-core1", "cix.pd.npu-core2",
    "cix.pd.npu-top", "cix.pd.isp0", "cix.pd.gpu",
};

static const struct { uint32_t id; const char *name; } clocks[] = {
    { 0x1e,  "cix.clk.gpu-400m" },
    { 0x1f,  "cix.clk.gpu-core" },
    { 0x20,  "cix.clk.gpu-stacks" },
    { 0x110, "cix.clk.gpu-200m" },
};

static void wr(const struct chan *c, uint32_t off, uint32_t v)
{
    *(volatile uint32_t *)(c->shm + off) = v;
}

static uint32_t rd(const struct chan *c, uint32_t off)
{
    return *(volatile uint32_t *)(c->shm + off);
}

/* The channel's writes before the ring; the kernel's call completes them
 * (dsb) before an SMC. */
static void barrier(void)
{
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

/* SCMI's status codes (§4.1.4), as the kernel's. */
static int64_t status(int32_t s)
{
    switch (s) {
    case 0:
        return 0;
    case -1:
        return ERR_NOSYS;           /* NOT_SUPPORTED */
    case -3:
        return ERR_RIGHTS;          /* DENIED */
    case -4:
        return ERR_NOTFOUND;
    case -6:
        return ERR_BUSY;
    case -2: case -5:
        return ERR_BADARG;          /* INVALID_PARAMETERS, OUT_OF_RANGE */
    default:
        return ERR_TIMEOUT;         /* the far side failed: comms, hardware, protocol */
    }
}

/* One command: n words of payload in; up to 'nout' words of the answer,
 * after its status, out. 0 or an error. */
static int64_t call(struct chan *c, uint32_t proto, uint32_t msg, const uint32_t *in, unsigned n,
                    uint32_t *out, unsigned nout)
{
    volatile uint32_t *st = (volatile uint32_t *)(c->shm + SMT_STATUS);
    uint32_t len;

    if (!c->shm || n > SMT_WORDS || nout + 1 > SMT_WORDS)
        return ERR_NOSYS;
    if (!pw_poll32(st, SMT_FREE, SMT_FREE, BUSY_US))
        return ERR_BUSY;
    c->token = (c->token + 1) & 0x3ff;
    wr(c, SMT_FLAGS, 0);                    /* polled, not an interrupt */
    wr(c, SMT_HEADER, msg | proto << 10 | (uint32_t)c->token << 18);
    for (unsigned i = 0; i < n; i++)
        wr(c, SMT_PAYLOAD + 4 * i, in[i]);
    wr(c, SMT_LENGTH, 4 + 4 * n);
    wr(c, SMT_STATUS, 0);                   /* busy: killed before the ring, hwdb frees it */
    barrier();
    if (c->doorbell) {
        *c->doorbell = 1;
    } else {
        uint64_t x[4] = { SMC_FID, 0, 0, 0 };
        int64_t e = pw_firmware_call(x);

        if (e)
            return e;
        if ((int64_t)x[0] != 0)
            return ERR_NOSYS;
    }
    if (!pw_poll32(st, SMT_FREE, SMT_FREE, BUSY_US))
        return ERR_TIMEOUT;
    barrier();
    if (*st & SMT_ERROR)
        return ERR_TIMEOUT;
    len = rd(c, SMT_LENGTH);
    if (len < 8 || len > SMT_SIZE - SMT_HEADER)
        return ERR_TIMEOUT;
    if (status((int32_t)rd(c, SMT_PAYLOAD)) != 0)
        return status((int32_t)rd(c, SMT_PAYLOAD));
    for (unsigned i = 0; i < nout; i++)
        out[i] = 4 * (i + 2) <= len - 4 ? rd(c, SMT_PAYLOAD + 4 * (i + 1)) : 0;
    return 0;
}

static void say_version(const char *what, int64_t e, uint32_t v)
{
    char line[96];
    unsigned n = 0;

    for (const char *s = "power: scmi: "; *s; s++)
        line[n++] = *s;
    for (const char *s = what; *s; s++)
        line[n++] = *s;
    if (e) {
        for (const char *s = " didn't answer"; *s; s++)
            line[n++] = *s;
    } else {
        for (const char *s = " version "; *s; s++)
            line[n++] = *s;
        for (int sh = 28; sh >= 0; sh -= 4)
            line[n++] = "0123456789abcdef"[v >> sh & 0xf];
    }
    line[n] = '\0';
    pw_say(line);
}

static bool init(void)
{
    uint32_t v = 0;
    int64_t e;
    bool any = false;

    for (unsigned i = 0; i < pw_ndev; i++) {
        if (pw_dev[i].be != &pw_scmi || !pw_dev[i].regs)
            continue;
        if (StrEqual(pw_dev[i].key, "CIXHA004"))
            mbox.shm = pw_dev[i].regs;
        else if (StrEqual(pw_dev[i].key, "CIXHA001") && pw_dev[i].size > DB)
            mbox.doorbell = (volatile uint32_t *)(pw_dev[i].regs + DB);
    }
    if (mbox.shm && mbox.doorbell) {
        e = call(&mbox, P_CLOCK, M_VERSION, nullptr, 0, &v, 1);
        say_version("clocks, by mailbox 6,", e, v);
        if (!e) {
            any = true;
            for (unsigned i = 0; i < sizeof clocks / sizeof clocks[0]; i++)
                if (pw_add(&pw_scmi, clocks[i].name, POWER_CLOCK, clocks[i].id, 0, 0) < 0)
                    return false;
        }
    } else {
        pw_say("power: scmi: no mailbox 6 in my registers: no clocks");
    }
    if ((smc.shm = pw_map(SMC_SHMEM, SMT_SIZE))) {
        e = call(&smc, P_POWER, M_VERSION, nullptr, 0, &v, 1);
        say_version("power domains, by SMC,", e, v);
        if (!e) {
            any = true;
            for (unsigned i = 0; i < sizeof domain_names / sizeof domain_names[0]; i++)
                if (pw_add(&pw_scmi, domain_names[i], POWER_DOMAIN, i, 0, 0) < 0)
                    return false;
        }
    } else {
        pw_say("power: scmi: no SMC channel in my registers: no power domains");
    }
    return any;
}

static int xlate(unsigned, const struct PowerRef *)
{
    return -1;                      /* ACPI names no SCMI resource */
}

static int64_t set(struct pw_res *r, bool on)
{
    if (r->kind == POWER_DOMAIN) {
        uint32_t in[3] = { 0, r->a, on ? PD_ON : PD_OFF };     /* flags 0: synchronous */

        return call(&smc, P_POWER, PD_STATE_SET, in, 3, nullptr, 0);
    }
    uint32_t in[2] = { r->a, on ? 1u : 0u };

    return call(&mbox, P_CLOCK, CLK_CONFIG_SET, in, 2, nullptr, 0);
}

static int64_t get(struct pw_res *r)
{
    uint32_t out[1] = { 0 };
    int64_t e;

    if (r->kind == POWER_DOMAIN) {
        e = call(&smc, P_POWER, PD_STATE_GET, &r->a, 1, out, 1);
        return e ? e : out[0] == PD_ON;
    }
    e = call(&mbox, P_CLOCK, CLK_ATTRIBUTES, &r->a, 1, out, 1);
    return e ? e : out[0] & 1;
}

/* CIXHA001 is every mailbox's: hwdb gives power mailbox 6 alone, by a quirk. */
static const char *const keys[] = { "CIXHA001", "CIXHA004", nullptr };
static const char *const gpu[] = { "gpu", "cix.pd.gpu", "cix.clk.gpu-core", "cix.clk.gpu-stacks",
                                   nullptr };
static const char *const *const sets[] = { gpu, nullptr };

const struct pw_backend pw_scmi = {
    .name = "scmi", .keys = keys, .init = init, .xlate = xlate, .set = set, .get = get,
    .sets = sets,
};
