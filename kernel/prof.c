/*
 * kernel/prof.c -- where the cycles go (kernel/prof.h)
 */
#include "prof.h"

#ifdef PLANAR_PROF
#include "arch.h"
#include "cpu.h"
#include "kprintf.h"

static const char *const names[P_POINTS] = {
    "trap in", "syscall", "syscall out", "trap out", "ipc posted", "wait", "schedule",
    "switch", "switched", "finished", "schedule out", "kick", "x1", "x2", "x3", "x4",
};

static struct prof_cpu {
    uint64_t sum[P_POINTS][P_POINTS], count[P_POINTS][P_POINTS];
    uint64_t at;
    int last;                   /* -1: no mark yet */
} prof[MAX_CPUS];

static bool on;

void prof_mark(enum prof_point p)
{
    unsigned long was = arch_irq_save();
    struct prof_cpu *c = &prof[this_cpu()->index];
    uint64_t now = arch_read_cycles();

    if (on && c->last >= 0) {
        c->sum[c->last][p] += now - c->at;
        c->count[c->last][p]++;
    }
    c->last = (int)p;
    c->at = arch_read_cycles();     /* the mark's own cost goes nowhere */
    arch_irq_restore(was);
}

void prof_reset(void)
{
    for (unsigned i = 0; i < MAX_CPUS; i++)
        prof[i] = (struct prof_cpu){ .last = -1 };
    on = true;
}

/* Every edge taken at least 'per'/4 times, with its share per 'per'
 * (round trips, say). */
void prof_report(const char *what, unsigned long per)
{
    uint64_t total = 0;

    on = false;
    kprintf("prof: %s, cycles per edge (per %lu):\n", what, per);
    for (unsigned a = 0; a < P_POINTS; a++)
        for (unsigned b = 0; b < P_POINTS; b++) {
            uint64_t sum = 0, count = 0;

            for (unsigned i = 0; i < ncpus; i++) {
                sum += prof[i].sum[a][b];
                count += prof[i].count[a][b];
            }
            if (count < per / 4)
                continue;
            total += sum;
            kprintf("prof:   %s -> %s: %llu each, %llu times, %llu per\n", names[a], names[b],
                    (unsigned long long)(sum / count), (unsigned long long)count,
                    (unsigned long long)(sum / per));
        }
    kprintf("prof:   all of them: %llu per\n", (unsigned long long)(total / per));
}
#endif
