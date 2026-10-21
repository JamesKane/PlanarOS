/*
 * conformance/leaser.c -- device leases, from the side of their owner
 *
 * Plays hwdb: started by the kernel with the device root as its second
 * argument. The first says where things are, packed: bits 0-23 a virtio
 * entropy device's registers (in 512-byte units, as virtio-mmio slots
 * are spaced; 0 if there's none), 24-33 its interrupt line, 34-63 the
 * kernel console's registers (in pages).
 *
 * Checks that the firmware's description is there to read (MapFirmware),
 * and readable by a child only once granted to it (GrantFirmware), and that a lease can't be had over memory, over the kernel's own
 * devices, over another lease, or with bad arguments. With a device,
 * grants it to conformance/virtiorng.c and checks it ran; then kills a
 * second driver with DMA outstanding and checks the Areas were
 * quarantined until the device was stopped and the lease revoked.
 * Last (v5-plan step 1), limits the lease's DMA and grants it to
 * conformance/doorbell.c's server, which shares its page with the app.
 * Exits 0 if every check passed, otherwise with the number of the
 * first that failed.
 */
#include <planar/exec.h>
#include <planar/lease.h>
#include <planar/syscalls.h>

#include "log.h"

constexpr uint64_t PAGE = 4096;
constexpr uint64_t MS = 1000000;

static unsigned step;

static int failed(const char *what)
{
    struct logbuf b = { 0 };

    log_str(&b, "leaser: check ");
    log_num(&b, step);
    log_str(&b, " failed: ");
    log_str(&b, what);
    KLog(b.text);
    return (int)step;
}

#define CHECK(cond)     do { step++; if (!(cond)) return failed(#cond); } while (0)

static void sleep_ms(uint64_t ms)
{
    ArmTimer(CurrentTime() + ms * MS, 30);
    Wait(1ULL << 30);
}

static uint64_t off;            /* the device's registers within the lease's page */

/* Start the driver in 'mode' on lease 'l'; its process handle. */
static int64_t driver(Handle l, uint64_t mode, int64_t sigbit)
{
    int64_t d = CreateProcess("virtiorng", l | mode << 32 | off / 512 << 40, sigbit, PROCF_HELD);

    if (d <= 0 || GrantLease(l, (Handle)d) != 0 || StartProcess((Handle)d) != 0)
        return -1;
    return d;
}

int main(uint64_t arg, uint64_t root_arg)
{
    Handle root = (Handle)root_arg;
    uint64_t slot = (arg & 0xffffff) * 512, console = (arg >> 34) * PAGE;
    uint64_t dev = slot & ~(PAGE - 1);
    uint32_t irq = (uint32_t)(arg >> 24) & 0x3ff, irqs[LEASE_IRQS + 1] = { irq, irq + 1 };
    uint64_t ram = (uint64_t)&arg & ~(PAGE - 1);
    struct FirmwareInfo fi;
    struct ProcessInfo pi;
    struct LeaseInfo li;
    volatile uint32_t *regs;
    uint64_t at;
    int64_t l, d;

    /* The firmware's description, read-only where it lies. */
    CHECK(root != 0);
    CHECK(MapFirmware(0, &fi) == ERR_BADHANDLE);
    CHECK(MapFirmware(root, &fi) == 0 && (fi.fi_Dtb || fi.fi_Rsdp));
    if (fi.fi_Dtb) {
        const uint8_t *d = (const uint8_t *)fi.fi_Dtb;
        uint32_t magic = (uint32_t)d[0] << 24 | d[1] << 16 | d[2] << 8 | d[3];
        uint32_t size = (uint32_t)d[4] << 24 | d[5] << 16 | d[6] << 8 | d[7];

        CHECK(magic == 0xd00dfeed && size == fi.fi_DtbSize);
    }
    if (fi.fi_Rsdp) {
        const char *r = (const char *)fi.fi_Rsdp;

        CHECK(r[0] == 'R' && r[1] == 'S' && r[2] == 'D' && r[7] == ' ');
    }

    /* A child reads them only once they're granted to it. */
    at = fi.fi_Dtb ? fi.fi_Dtb : fi.fi_Rsdp;
    d = CreateProcess("fwreader", at, 23, PROCF_HELD);
    CHECK(d > 0 && StartProcess((Handle)d) == 0);
    Wait(1ULL << 23);
    CHECK(QueryProcess((Handle)d, &pi) == 0 && pi.pi_State == PROCS_FAULTED &&
          pi.pi_Fault == PFAULT_READ && pi.pi_FaultAddress == at);
    CHECK(CloseHandle((Handle)d) == 0);
    d = CreateProcess("fwreader", at, 23, PROCF_HELD);
    CHECK(d > 0);
    CHECK(GrantFirmware(0, (Handle)d) == ERR_BADHANDLE);
    CHECK(GrantFirmware(root, root) == ERR_BADHANDLE);     /* not a process */
    CHECK(GrantFirmware(root, (Handle)d) == 0 && StartProcess((Handle)d) == 0);
    Wait(1ULL << 23);
    CHECK(QueryProcess((Handle)d, &pi) == 0 && pi.pi_State == PROCS_EXITED &&
          pi.pi_ExitCode == 0);
    CHECK(CloseHandle((Handle)d) == 0);
    CHECK(GrantFirmware(root, (Handle)d) == ERR_BADHANDLE); /* closed */

    /* What no lease may cover, and bad arguments. */
    CHECK(CreateLease(0, 0x1000, PAGE, nullptr, 0) == ERR_BADHANDLE);
    CHECK(CreateLease(root, ram, PAGE, nullptr, 0) == ERR_RIGHTS);          /* memory */
    if (console)
        CHECK(CreateLease(root, console, PAGE, nullptr, 0) == ERR_RIGHTS); /* the kernel's */
    CHECK(CreateLease(root, 0x1000, 0, nullptr, 0) == ERR_BADARG);
    CHECK(CreateLease(root, 0x1001, PAGE, nullptr, 0) == ERR_BADARG);
    CHECK(CreateLease(root, ~0ULL & ~(PAGE - 1), 2 * PAGE, nullptr, 0) == ERR_BADARG);
    CHECK(CreateLease(root, 0x1000, PAGE, irqs, LEASE_IRQS + 1) == ERR_BADARG); /* too many */
    irqs[1] = irqs[0];
    CHECK(CreateLease(root, 0x1000, PAGE, irqs, 2) == ERR_BADARG);          /* twice */
    CHECK(CreateLease(root, 0x1000, PAGE, (const uint32_t *)8, 1) == ERR_BADADDR);
    off = slot - dev;
    if (!slot) {
        KLog("leaser: no test device; refusals only");
        return 0;
    }

    /* A lease, and none over it or its line. */
    l = CreateLease(root, dev, PAGE, &irq, 1);
    CHECK(l > 0);
    CHECK(CreateLease(root, dev, PAGE, nullptr, 0) == ERR_EXISTS);
    irqs[0] = irq;
    CHECK(CreateLease(root, dev + 0x10000000, PAGE, irqs, 1) == ERR_EXISTS);
    CHECK(QueryLease((Handle)l, &li) == 0 && li.li_State == LEASES_IDLE &&
          li.li_Base == dev && li.li_Irqs == 1);

    /* Its owner may map it while nobody holds it. */
    CHECK(MapLease((Handle)l, (void **)&regs) == 0 && regs[off / 4] == 0x74726976);
    CHECK(BindIrq((Handle)l, 0, 20) == ERR_BADHANDLE);     /* only a grantee binds */

    /* A driver that runs to the end. */
    d = driver((Handle)l, 0, 20);
    CHECK(d > 0);
    CHECK(GrantLease((Handle)l, (Handle)d) == ERR_BUSY);
    CHECK(RevokeLease((Handle)l) == ERR_BUSY);
    CHECK(CloseHandle((Handle)l) == ERR_BUSY);
    Wait(1ULL << 20);
    CHECK(QueryProcess((Handle)d, &pi) == 0 && pi.pi_State == PROCS_EXITED &&
          pi.pi_ExitCode == 0);
    CHECK(CloseHandle((Handle)d) == 0);
    CHECK(QueryLease((Handle)l, &li) == 0 && li.li_State == LEASES_STOPPING &&
          li.li_QuarantinedPages == 0 && li.li_DmaPages == 0);
    CHECK(RevokeLease((Handle)l) == 0);

    /* One killed with its request outstanding. */
    d = driver((Handle)l, 1, 21);
    CHECK(d > 0);
    for (unsigned i = 0; i < 1000 && (QueryLease((Handle)l, &li), li.li_DmaPages < 2); i++)
        sleep_ms(1);
    CHECK(li.li_DmaPages == 2);
    sleep_ms(5);                            /* its request at the device */
    CHECK(KillProcess((Handle)d) == 0);
    Wait(1ULL << 21);
    CHECK(QueryLease((Handle)l, &li) == 0 && li.li_State == LEASES_STOPPING &&
          li.li_QuarantinedPages == 2 && li.li_DmaPages == 0);
    CHECK(CloseHandle((Handle)d) == 0);
    /* Stop the device ourselves, then take the lease back. */
    CHECK(MapLease((Handle)l, (void **)&regs) == 0);
    regs[(off + 0x70) / 4] = 0;
    while (regs[(off + 0x70) / 4] != 0)
        ;
    CHECK(RevokeLease((Handle)l) == 0);
    CHECK(QueryLease((Handle)l, &li) == 0 && li.li_State == LEASES_IDLE &&
          li.li_QuarantinedPages == 0);

    /* Granted to one never started: closing the process stops it. */
    d = CreateProcess("virtiorng", (Handle)l, 22, PROCF_HELD);
    CHECK(d > 0 && GrantLease((Handle)l, (Handle)d) == 0);
    CHECK(CloseHandle((Handle)d) == 0);
    Wait(1ULL << 22);
    CHECK(RevokeLease((Handle)l) == 0);

    /* A DMA limit with room for one page below it: the lowest free page,
     * held while the server is made, then let go. */
    void *low;
    int64_t lowa, s, c;

    CHECK(LimitLeaseDma((Handle)l, 1) == ERR_BADARG);
    CHECK(LimitLeaseDma(root, PAGE) == ERR_BADHANDLE);
    lowa = AllocArea(PAGE, 0, &low);
    CHECK(lowa > 0);
    s = CreateProcess("doorbell", 1 | off / 512 << 8 | (uint64_t)l << 32, 24, PROCF_HELD);
    CHECK(s > 0);
    CHECK(LimitLeaseDma((Handle)l, (uint64_t)low + PAGE) == 0);
    CHECK(QueryLease((Handle)l, &li) == 0 && li.li_DmaLimit == (uint64_t)low + PAGE);
    CHECK(GrantLease((Handle)l, (Handle)s) == 0);
    CHECK(LimitLeaseDma((Handle)l, 0) == ERR_BUSY);
    /* Its app, which reads the device through the shared page, then
     * faults on it once the server has gone: made now, and started once
     * the server has its page, so it can't take that page first. */
    c = CreateProcess("doorbell", 2 | (uint64_t)FindTask(nullptr) << 32, 25, PROCF_HELD);
    CHECK(c > 0);
    CHECK(FreeArea((Handle)lowa) == 0);
    CHECK(StartProcess((Handle)s) == 0);
    for (unsigned i = 0; i < 1000 && (QueryLease((Handle)l, &li), li.li_DmaPages < 1); i++)
        sleep_ms(1);
    CHECK(li.li_DmaPages == 1);
    CHECK(StartProcess((Handle)c) == 0);
    for (uint64_t got = 0; got != (1ULL << 24 | 1ULL << 25);)
        got |= Wait(1ULL << 24 | 1ULL << 25);
    CHECK(QueryProcess((Handle)s, &pi) == 0 && pi.pi_State == PROCS_EXITED &&
          pi.pi_ExitCode == 0);
    CHECK(QueryProcess((Handle)c, &pi) == 0 && pi.pi_State == PROCS_FAULTED &&
          pi.pi_Fault == PFAULT_READ && pi.pi_FaultAddress == dev + off);
    CHECK(CloseHandle((Handle)s) == 0 && CloseHandle((Handle)c) == 0);
    CHECK(QueryLease((Handle)l, &li) == 0 && li.li_State == LEASES_STOPPING &&
          li.li_Shares == 0 && li.li_QuarantinedPages == 1);
    CHECK(RevokeLease((Handle)l) == 0);
    CHECK(LimitLeaseDma((Handle)l, 0) == 0);

    CHECK(CloseHandle((Handle)l) == 0);
    CHECK(QueryLease((Handle)l, &li) == ERR_BADHANDLE);
    return 0;
}
