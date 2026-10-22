/*
 * conformance/fpu.c -- FP and vector registers survive preemption and
 * moving between CPUs
 *
 * Three tasks each fill all 32 FP and all 32 vector registers with values
 * of their own and set the rounding mode, spin for several quanta while
 * other tasks (and other copies of this program) do the same, and check
 * every register came back, along with the ISA's other per-task register
 * (fpregs.h). The kernel switches this state lazily, so it must notice
 * whose it is on every CPU. The argument is how many clock
 * ticks make 10 ms. Exits with the number of rounds that went wrong.
 */
#include <planar/exec.h>

#include "fpregs.h"
#include "log.h"

constexpr unsigned ROUNDS = 12;
constexpr unsigned TASKS = 3;

static uint64_t ticks_10ms;
static Handle main_task;
static uint64_t bad[TASKS];
static uint64_t vout[TASKS][32 * FPREGS_MAX_VEC_BYTES / 8];

static void rounds(uint64_t t)
{
    uint64_t in[32], fout[32], misc[2], seed = FindTask(nullptr), vb = fpregs_vec_bytes();

    for (unsigned round = 0; round < ROUNDS; round++) {
        bool ok = true;
        uint64_t mode = round % fpregs_modes(), extra = fpregs_extra(seed + round);

        for (unsigned r = 0; r < 32; r++)
            in[r] = seed << 32 | (uint64_t)round << 8 | r;
        fpregs_hold(in, fout, vout[t], misc, mode, extra, ticks_10ms);
        ok &= misc[0] == mode && misc[1] == extra;
        for (unsigned r = 0; r < 32; r++) {
            ok &= fout[r] == in[r];
            for (unsigned e = 0; e < vb / 8; e++)
                ok &= vout[t][r * (vb / 8) + e] == fpregs_vec_expect(in[r], e);
        }
        bad[t] += !ok;
    }
}

static void worker(uint64_t t)
{
    rounds(t);
    Signal(main_task, 1ULL << (20 + t));
    ExitTask(0);
}

int main(uint64_t ticks)
{
    uint64_t want = 0, got = 0, total = 0;
    if (fpregs_vec_bytes() > FPREGS_MAX_VEC_BYTES) {
        KLog("fpu: vector registers too long for this test");
        return 99;
    }
    ticks_10ms = ticks;
    main_task = FindTask(nullptr);
    for (unsigned t = 1; t < TASKS; t++) {
        want |= 1ULL << (20 + t);
        CreateTask(worker, t);
    }
    rounds(0);
    while (got != want)
        got |= Wait(want);
    for (unsigned t = 0; t < TASKS; t++)
        total += bad[t];
    KPrintF("fpu: %lu rounds, %lu bad, vector registers %lu bits", (uint64_t)(TASKS * ROUNDS),
            total, (uint64_t)(fpregs_vec_bytes() * 8));
    return (int)total;
}
