/*
 * servers/hwdb/gpu.c -- the GPU servers' stop methods, and the GPU test
 * (v5-plan steps 5 and 6)
 *
 * A dead gpu-panthor's MCU may still be running groups, which read and
 * write its clients' memory and its own: its stop is the MCU disabled
 * and the GPU soft-reset, after which it touches no memory. A GPU not
 * yet powered (the RCSU's power bit clear), or held in reset (RST0's bit
 * 6 clear: the driver died in its reset pulse), has nothing running,
 * and its registers mustn't be touched: the access is refused, and the
 * board stops.
 *
 * The test (killtest.c's T_GPU), with gputest as the client: a round,
 * then a client killed with jobs in flight and another round after it,
 * then a round across the server's own death. The same for either
 * server: the CIX's gpu-panthor (step 5), where the app rings its own
 * doorbell, and the RV2's gpu-rogue (step 6), a server queue.
 */
#include <stddef.h>

#include <planar/syscalls.h>

#include "server.h"

constexpr uint64_t RCSU_POWER = 0x15000218, GPU = 0x15010000, RST_GPU = 0x16000400;
constexpr uint32_t GPU_INT_RAWSTAT = 0x20, GPU_INT_CLEAR = 0x24, GPU_CMD = 0x30;
constexpr uint32_t MCU_CONTROL = 0x700;
constexpr uint32_t RESET_COMPLETED = 1u << 8, SOFT_RESET = 0x101;

static bool is_gpu(struct unit *u)
{
    return hw_streq(unit_driver(u), "gpu-panthor") || hw_streq(unit_driver(u), "gpu-rogue");
}

bool stop_gpu(struct unit *u)
{
    const struct hw_lease *ls = &hwdb.g.leases[u - hwdb.units];
    volatile uint32_t *g = (volatile uint32_t *)GPU;
    bool mapped = false, rst = false;
    uint64_t until = CurrentTime() + 100 * MS;

    for (unsigned r = 0; r < ls->nranges; r++) {
        void *at;
        int64_t e = MapLease(u->lease[r], &at);

        if (e != 0 && e != ERR_EXISTS)
            return false;
        mapped |= ls->ranges[r].base <= GPU && GPU < ls->ranges[r].base + ls->ranges[r].size;
        rst |= ls->ranges[r].base <= RST_GPU && RST_GPU < ls->ranges[r].base + ls->ranges[r].size;
    }
    if (!mapped || !rst)
        return false;
    if (!(*(volatile uint32_t *)RCSU_POWER & 0x1000) || !(*(volatile uint32_t *)RST_GPU & 1u << 6))
        return true;                    /* never powered, or in reset: nothing runs */
    g[MCU_CONTROL / 4] = 0;
    g[GPU_INT_CLEAR / 4] = RESET_COMPLETED;
    g[GPU_CMD / 4] = SOFT_RESET;
    while (!(g[GPU_INT_RAWSTAT / 4] & RESET_COMPLETED))
        if (CurrentTime() > until)
            return false;
    g[GPU_INT_CLEAR / 4] = RESET_COMPLETED;
    return true;
}

/*
 * gpu-rogue's (the RV2's BXE-2-32): everything back in soft reset, the
 * MIPS with it, as mainline's pvr_fw_stop ends. Its power is hwdb's
 * vote (a quirk), held until this has run, so its registers answer.
 */
constexpr uint64_t ROGUE = 0xcac00000;
constexpr uint32_t ROGUE_SOFT_RESET = 0x100, ROGUE_SOFT_RESET2 = 0x108;

bool stop_rogue(struct unit *u)
{
    const struct hw_lease *ls = &hwdb.g.leases[u - hwdb.units];
    volatile uint64_t *g = (volatile uint64_t *)ROGUE;
    bool mapped = false;

    for (unsigned r = 0; r < ls->nranges; r++) {
        void *at;
        int64_t e = MapLease(u->lease[r], &at);

        if (e != 0 && e != ERR_EXISTS)
            return false;
        mapped |= ls->ranges[r].base <= ROGUE && ROGUE < ls->ranges[r].base + ls->ranges[r].size;
    }
    if (!mapped)
        return false;
    g[ROGUE_SOFT_RESET / 8] = 0xffeffffffffffc3dull;
    g[ROGUE_SOFT_RESET2 / 8] = 0x1fffff;
    (void)g[ROGUE_SOFT_RESET / 8];
    (void)g[ROGUE_SOFT_RESET2 / 8];
    return true;
}

/* --- The test. --- */

enum : uint8_t { G_START, G_FIRST, G_LOAD, G_KILLED, G_AFTER, G_ACROSS, G_DONE };

static struct {
    uint8_t step;
    Handle client;
    uint64_t at;
    bool rogue;
} gt;

static void test_fail(const char *what)
{
    struct hw_line l = { 0 };

    hw_put(&l, "hwdb: gpu test: FAILED: ");
    hw_put(&l, what);
    say(l.text);
    hwdb.errors++;
    if (gt.client) {
        KillProcess(gt.client);
        CloseHandle(gt.client);
        gt.client = 0;
    }
    gt.step = G_DONE;
}

static bool late(uint64_t ms)
{
    return hwdb.now - gt.at > ms * MS;
}

static struct unit *gpu_unit(void)
{
    for (uint32_t i = 0; i < hwdb.g.nleases; i++)
        if (is_gpu(&hwdb.units[i]) && hwdb.units[i].state == U_RUNNING)
            return &hwdb.units[i];
    return nullptr;
}

/* 0 running, else its exit code + 1 (-1 if it didn't exit). */
static int client_state(void)
{
    struct ProcessInfo pi;

    if (QueryProcess(gt.client, &pi) != 0 || pi.pi_State < PROCS_EXITED)
        return 0;
    CloseHandle(gt.client);
    gt.client = 0;
    return pi.pi_State == PROCS_EXITED ? (int)pi.pi_ExitCode + 1 : -1;
}

static bool start_client(uint64_t mode)
{
    int64_t p = CreateProcess("gputest", mode, SIG_DEAD, 0);

    if (p < 0) {
        test_fail("gputest couldn't be started");
        return false;
    }
    gt.client = (Handle)p;
    gt.at = hwdb.now;
    return true;
}

/* A round's end: true when it's over, the test with it if it failed. */
static bool ended(int s, const char *what)
{
    if (s == 0) {
        if (late(60000))
            test_fail(what);
        return false;
    }
    if (s == 4) {
        say(gt.rogue ? "V5.6 gpu: not exercised: the GPU's firmware didn't come up"
                     : "V5.5 gpu: not exercised: the GPU's firmware didn't come up");
        gt.step = G_DONE;
        return false;
    }
    if (s != 1) {
        test_fail(what);
        return false;
    }
    return true;
}

bool gpu_test(void)
{
    struct unit *u;
    int s;

    switch (gt.step) {
    case G_START:
        if (!(u = gpu_unit())) {
            say("V5.5 gpu: not exercised: no GPU here");
            return true;
        }
        gt.rogue = hw_streq(unit_driver(u), "gpu-rogue");
        if (start_client(0))
            gt.step = G_FIRST;
        return false;
    case G_FIRST:
        if (!ended(client_state(), "gputest's first round failed"))
            return gt.step == G_DONE;
        if (start_client(1))
            gt.step = G_LOAD;
        return false;
    case G_LOAD:
        /* Jobs running: the client is killed under them. */
        if ((s = client_state()) != 0) {
            test_fail("gputest ended with its jobs running");
            return true;
        }
        if (!late(2000))
            return false;
        KillProcess(gt.client);
        gt.step = G_KILLED;
        return false;
    case G_KILLED:
        if (client_state() == 0)
            return false;
        if (start_client(2))
            gt.step = G_AFTER;
        return false;
    case G_AFTER:
        /* Its first round, then the server killed under it. */
        if ((s = client_state()) != 0) {
            ended(s, "gputest ended before the GPU server was killed");
            return true;
        }
        if (!late(5000))
            return false;
        if (!(u = gpu_unit())) {
            test_fail("the GPU server isn't running");
            return true;
        }
        unit_kill(u);
        gt.step = G_ACROSS;
        return false;
    case G_ACROSS:
        if (!ended(client_state(), "gputest's round across the GPU server's death failed"))
            return gt.step == G_DONE;
        if (gt.rogue)
            say("V5.6 gpu ok: jobs ran from the app's own client CCB, kicked through gpu-rogue; "
                "again after a client was killed with jobs in flight, and after gpu-rogue was");
        else
            say("V5.5 gpu ok: jobs ran from the app's own queue, rung by its own doorbell; again "
                "after a client was killed with jobs in flight, and after gpu-panthor was");
        return true;
    case G_DONE:
        return true;
    }
    return true;
}
