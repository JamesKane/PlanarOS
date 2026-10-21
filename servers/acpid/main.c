/*
 * servers/acpid/main.c -- acpid on Planar: hwdb's protocol, windows, time
 *
 * hwdb starts it with a token as its argument and the firmware's tables
 * granted to it (planar/acpid.h). It says hello on "hwdb.acpi", which
 * tells it the RSDP, loads the namespace (walk.c), sends each device,
 * says it's done, and then waits for hwdb to go.
 *
 * Every window the AML or ACPICA touches is asked of hwdb once, by whole
 * pages, and its answer kept: the firmware's tables (already mapped), a
 * lease (mapped now, kept until acpid ends), or a refusal.
 *
 * In hwdb's test mode (ACPID_ARG_PROBE) it then asks for windows hwdb
 * must refuse: the GIC's registers, its own stack, which is memory, and
 * each device's registers, which must be refused if a driver or the
 * kernel has them and may be leased if not; and the RSDP's page, which
 * is the tables. It checks each answer, and tells hwdb whether all were
 * right (ACPID_PROBED).
 */
#include <planar/exec.h>
#include <planar/lease.h>
#include <planar/syscalls.h>

#include "acpid.h"

constexpr int64_t SIG_HWDB = 16;
constexpr int64_t SIG_TIMER = 17;
constexpr uint64_t PAGE = 4096;
constexpr unsigned WINDOWS = 64;

void *memcpy(void *restrict dst, const void *restrict src, size_t n);

static struct MsgPort hwdb;
static struct AcpidMsg *m;          /* the one message: acpid asks one thing at a time */
static uint32_t token;
static uint64_t rsdp;

static struct {
    uint64_t base, size;
    bool refused;
} windows[WINDOWS];
static unsigned nwindows;
static uint64_t regs[WINDOWS];      /* each device's first registers' page, for the probe */
static unsigned nregs;

static bool ask(uint32_t op)
{
    m->am_Msg.mn_Length = sizeof *m;
    m->am_Op = op;
    m->am_Token = token;
    return DoMsg(&hwdb, &m->am_Msg) == 0 && m->am_Msg.mn_Error == 0;
}

volatile void *acpid_map(uint64_t pa, uint64_t len, bool *refused)
{
    uint64_t base = pa & ~(PAGE - 1), end = (pa + len + PAGE - 1) & ~(PAGE - 1);
    bool no = true;

    if (len == 0 || pa + len < pa)
        return nullptr;
    for (unsigned i = 0; i < nwindows; i++)
        if (pa >= windows[i].base && pa + len <= windows[i].base + windows[i].size) {
            *refused = windows[i].refused;
            return windows[i].refused ? nullptr : (volatile void *)pa;
        }
    m->am_Base = base;
    m->am_Size = end - base;
    m->am_Count = 0;
    if (ask(ACPID_REGION)) {
        void *at;

        if (m->am_Count == ACPID_TABLES)
            no = false;
        else if (m->am_Count == ACPID_LEASED)
            no = MapLease(m->am_Lease, &at) != 0 || at != (void *)base;
    }
    if (nwindows < WINDOWS)
        windows[nwindows++] = (typeof(windows[0])){ base, end - base, no };
    *refused = no;
    return no ? nullptr : (volatile void *)pa;
}

uint64_t acpid_rsdp(void)
{
    return rsdp;
}

uint64_t acpid_now(void)
{
    return CurrentTime();
}

void acpid_sleep(uint64_t ns)
{
    uint64_t until = CurrentTime() + ns;

    while (CurrentTime() < until) {
        ArmTimer(until, SIG_TIMER);
        Wait(1ULL << SIG_TIMER);
    }
    ArmTimer(0, 0);
}

void acpid_say(const char *line)
{
    KLog(line);
}

/* Its errors and warnings; its news ("ACPI: " and the tables it found,
 * which the kernel has listed) isn't the console's. */
void acpid_say_acpica(const char *line)
{
    if (!(line[0] == 'A' && line[1] == 'C' && line[2] == 'P' && line[3] == 'I' &&
          line[4] == ':'))
        KLog(line);
}

static void send(const struct AcpidDevice *d, void *)
{
    bool seen = false;

    memcpy(&m->am_Device, d, sizeof *d);
    if (!ask(ACPID_DEVICE))
        acpid_say("acpid: hwdb refused a device");
    for (unsigned i = 0; i < d->ad_NRes; i++)
        if (d->ad_Res[i].ar_Kind == ACPID_R_MMIO) {
            uint64_t page = d->ad_Res[i].ar_Base & ~(PAGE - 1);

            for (unsigned k = 0; k < nregs; k++)
                seen |= regs[k] == page;
            if (!seen && nregs < WINDOWS)
                regs[nregs++] = page;
            break;
        }
}

/* --- The probe. --- */

static uint32_t answer(uint64_t page)
{
    m->am_Base = page;
    m->am_Size = PAGE;
    m->am_Count = 1;
    return ask(ACPID_REGION) ? m->am_Count : ACPID_REFUSED;
}

static void num(char *line, unsigned *n, uint64_t v)
{
    char d[24];
    unsigned k = 0;

    do
        d[k++] = (char)('0' + v % 10);
    while ((v /= 10) && k < sizeof d);
    while (k && *n < 190)
        line[(*n)++] = d[--k];
}

static void put(char *line, unsigned *n, const char *s)
{
    while (*s && *n < 190)
        line[(*n)++] = *s++;
}

static bool probe(void)
{
    uint64_t gic = acpid_gic(), stack = (uint64_t)&gic & ~(PAGE - 1);
    unsigned leased = 0, refused = 0, n = 0;
    bool ok = true;
    char line[200];

    if (gic)
        ok &= answer(gic & ~(PAGE - 1)) == ACPID_REFUSED;
    ok &= answer(stack) == ACPID_REFUSED;
    ok &= answer(rsdp & ~(PAGE - 1)) == ACPID_TABLES;
    for (unsigned i = 0; i < nregs; i++) {
        void *at;

        switch (answer(regs[i])) {
        case ACPID_LEASED:
            ok &= MapLease(m->am_Lease, &at) == 0 && at == (void *)regs[i];
            if (nwindows < WINDOWS)
                windows[nwindows++] = (typeof(windows[0])){ regs[i], PAGE, false };
            leased++;
            break;
        case ACPID_REFUSED:
            refused++;
            break;
        default:
            ok = false;     /* a device's registers aren't the tables */
            break;
        }
    }
    put(line, &n, ok ? "acpid: probe ok: " : "acpid: probe FAILED: ");
    put(line, &n, gic ? "the GIC's registers and " : "");
    put(line, &n, "its own stack refused, the RSDP's page the tables; of ");
    num(line, &n, nregs);
    put(line, &n, " devices' register pages, ");
    num(line, &n, leased);
    put(line, &n, " leased and ");
    num(line, &n, refused);
    put(line, &n, " refused");
    line[n] = '\0';
    acpid_say(line);
    return ok;
}

int main(uint64_t arg)
{
    uint64_t start = CurrentTime();
    char line[80] = "acpid: the namespace loaded and described in ";
    unsigned n, k = 45;

    token = (uint32_t)arg;
    if (ConnectPort(&hwdb, "hwdb.acpi", SIG_HWDB, 2, 2 * sizeof *m + 4096) != 0 ||
        !(m = AllocMsg(&hwdb, sizeof *m)))
        return 1;
    if (!ask(ACPID_HELLO) || !(rsdp = m->am_Base))
        return 2;
    if (!acpid_load())
        return 3;
    n = acpid_walk(send, nullptr);
    acpid_say_count();
    num(line, &k, (CurrentTime() - start) / 1000000);
    put(line, &k, " ms");
    line[k] = '\0';
    acpid_say(line);
    m->am_Count = n;
    if (!ask(ACPID_DONE))
        return 4;
    if (arg & ACPID_ARG_PROBE) {
        m->am_Count = probe();
        if (!ask(ACPID_PROBED) || !m->am_Count)
            return 5;
    }

    /* Nothing more is asked of it yet (the CIX's power resources will be). */
    while (!PortGone(&hwdb))
        Wait(1ULL << SIG_HWDB);
    return 0;
}
