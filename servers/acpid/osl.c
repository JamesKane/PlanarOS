/*
 * servers/acpid/osl.c -- ACPICA's OS layer (acpiosxf.h), over acpid.h
 *
 * The same on Planar and the host: what differs is behind acpid.h's
 * platform calls. acpid is one task and ACPICA runs only when acpid
 * calls it, so a lock is never contended and a semaphore that would
 * have to wait never will be signalled: it fails at once (AE_TIME).
 * Deferred work (AcpiOsExecute: Notify handlers) is queued and run when
 * ACPICA asks for its events to complete, or acpid between calls.
 *
 * There are no I/O ports on Planar's ISAs, and PCI configuration space
 * belongs to the pci server (step 5): both are refused, and counted.
 */
#include "acpi.h"

#include "acpid.h"

/* --- Start and tables. --- */

ACPI_STATUS AcpiOsInitialize(void)
{
    return AE_OK;
}

ACPI_STATUS AcpiOsTerminate(void)
{
    return AE_OK;
}

ACPI_PHYSICAL_ADDRESS AcpiOsGetRootPointer(void)
{
    return acpid_rsdp();
}

ACPI_STATUS AcpiOsPredefinedOverride(const ACPI_PREDEFINED_NAMES *, ACPI_STRING *value)
{
    *value = NULL;
    return AE_OK;
}

ACPI_STATUS AcpiOsTableOverride(ACPI_TABLE_HEADER *, ACPI_TABLE_HEADER **table)
{
    *table = NULL;
    return AE_OK;
}

ACPI_STATUS AcpiOsPhysicalTableOverride(ACPI_TABLE_HEADER *, ACPI_PHYSICAL_ADDRESS *at,
                                        UINT32 *len)
{
    *at = 0;
    *len = 0;
    return AE_OK;
}

/* --- Locks and semaphores: one task. --- */

struct sem {
    UINT32 units, max;
};

ACPI_STATUS AcpiOsCreateLock(ACPI_SPINLOCK *lock)
{
    static int the_lock;

    *lock = &the_lock;
    return AE_OK;
}

void AcpiOsDeleteLock(ACPI_SPINLOCK)
{
}

ACPI_CPU_FLAGS AcpiOsAcquireLock(ACPI_SPINLOCK)
{
    return 0;
}

void AcpiOsReleaseLock(ACPI_SPINLOCK, ACPI_CPU_FLAGS)
{
}

ACPI_STATUS AcpiOsCreateSemaphore(UINT32 max, UINT32 initial, ACPI_SEMAPHORE *out)
{
    struct sem *s;

    if (!out || initial > max)
        return AE_BAD_PARAMETER;
    if (!(s = malloc(sizeof *s)))
        return AE_NO_MEMORY;
    *s = (struct sem){ initial, max };
    *out = s;
    return AE_OK;
}

ACPI_STATUS AcpiOsDeleteSemaphore(ACPI_SEMAPHORE h)
{
    if (!h)
        return AE_BAD_PARAMETER;
    free(h);
    return AE_OK;
}

ACPI_STATUS AcpiOsWaitSemaphore(ACPI_SEMAPHORE h, UINT32 units, UINT16)
{
    struct sem *s = h;

    if (!s)
        return AE_BAD_PARAMETER;
    if (s->units < units)
        return AE_TIME;             /* nobody else could signal it */
    s->units -= units;
    return AE_OK;
}

ACPI_STATUS AcpiOsSignalSemaphore(ACPI_SEMAPHORE h, UINT32 units)
{
    struct sem *s = h;

    if (!s || s->units + units > s->max)
        return AE_LIMIT;
    s->units += units;
    return AE_OK;
}

/* --- Memory. --- */

void *AcpiOsAllocate(ACPI_SIZE size)
{
    return malloc(size);
}

void AcpiOsFree(void *p)
{
    free(p);
}

void *AcpiOsMapMemory(ACPI_PHYSICAL_ADDRESS at, ACPI_SIZE len)
{
    bool refused = false;

    return (void *)acpid_map(at, len, &refused);
}

void AcpiOsUnmapMemory(void *, ACPI_SIZE)
{
}

/* The address space is one, and identity-mapped (VQ3). */
ACPI_STATUS AcpiOsGetPhysicalAddress(void *p, ACPI_PHYSICAL_ADDRESS *at)
{
    *at = (ACPI_PHYSICAL_ADDRESS)(uintptr_t)p;
    return AE_OK;
}

ACPI_STATUS AcpiOsReadMemory(ACPI_PHYSICAL_ADDRESS at, UINT64 *value, UINT32 bits)
{
    bool refused = false;
    volatile void *p = acpid_map(at, bits / 8, &refused);

    if (!p)
        return AE_ACCESS;
    switch (bits) {
    case 8:  *value = *(volatile uint8_t *)p; break;
    case 16: *value = *(volatile uint16_t *)p; break;
    case 32: *value = *(volatile uint32_t *)p; break;
    case 64: *value = *(volatile uint64_t *)p; break;
    default: return AE_BAD_PARAMETER;
    }
    return AE_OK;
}

ACPI_STATUS AcpiOsWriteMemory(ACPI_PHYSICAL_ADDRESS at, UINT64 value, UINT32 bits)
{
    bool refused = false;
    volatile void *p = acpid_map(at, bits / 8, &refused);

    if (!p)
        return AE_ACCESS;
    switch (bits) {
    case 8:  *(volatile uint8_t *)p = (uint8_t)value; break;
    case 16: *(volatile uint16_t *)p = (uint16_t)value; break;
    case 32: *(volatile uint32_t *)p = (uint32_t)value; break;
    case 64: *(volatile uint64_t *)p = value; break;
    default: return AE_BAD_PARAMETER;
    }
    return AE_OK;
}

/* --- Ports and PCI: not here. --- */

ACPI_STATUS AcpiOsReadPort(ACPI_IO_ADDRESS, UINT32 *value, UINT32)
{
    *value = UINT32_MAX;
    return AE_SUPPORT;
}

ACPI_STATUS AcpiOsWritePort(ACPI_IO_ADDRESS, UINT32, UINT32)
{
    return AE_SUPPORT;
}

ACPI_STATUS AcpiOsReadPciConfiguration(ACPI_PCI_ID *, UINT32, UINT64 *value, UINT32)
{
    *value = UINT64_MAX;
    return AE_SUPPORT;
}

ACPI_STATUS AcpiOsWritePciConfiguration(ACPI_PCI_ID *, UINT32, UINT64, UINT32)
{
    return AE_SUPPORT;
}

/* --- Interrupts: hardware-reduced ACPI has no SCI. --- */

ACPI_STATUS AcpiOsInstallInterruptHandler(UINT32, ACPI_OSD_HANDLER, void *)
{
    return AE_SUPPORT;
}

ACPI_STATUS AcpiOsRemoveInterruptHandler(UINT32, ACPI_OSD_HANDLER)
{
    return AE_SUPPORT;
}

/* --- Deferred work, time. --- */

constexpr unsigned DEFERRED = 32;

static struct {
    ACPI_OSD_EXEC_CALLBACK fn;
    void *ctx;
} deferred[DEFERRED];
static unsigned ndeferred;

ACPI_THREAD_ID AcpiOsGetThreadId(void)
{
    return 1;
}

ACPI_STATUS AcpiOsExecute(ACPI_EXECUTE_TYPE, ACPI_OSD_EXEC_CALLBACK fn, void *ctx)
{
    if (ndeferred == DEFERRED)
        return AE_NO_MEMORY;
    deferred[ndeferred].fn = fn;
    deferred[ndeferred++].ctx = ctx;
    return AE_OK;
}

void AcpiOsWaitEventsComplete(void)
{
    for (unsigned i = 0; i < ndeferred; i++)
        deferred[i].fn(deferred[i].ctx);
    ndeferred = 0;
}

void AcpiOsSleep(UINT64 ms)
{
    acpid_sleep(ms * 1000000);
}

void AcpiOsStall(UINT32 us)
{
    uint64_t until = acpid_now() + (uint64_t)us * 1000;

    while (acpid_now() < until)
        ;
}

UINT64 AcpiOsGetTimer(void)
{
    return acpid_now() / 100;       /* ACPI's 100 ns ticks */
}

ACPI_STATUS AcpiOsSignal(UINT32 function, void *)
{
    if (function == ACPI_SIGNAL_FATAL)
        acpid_say("acpid: the AML signalled a fatal error");
    return AE_OK;
}

ACPI_STATUS AcpiOsEnterSleep(UINT8, UINT32, UINT32)
{
    return AE_OK;
}

/* --- Output: ACPICA's messages, a line at a time. --- */

int vsnprintf(char *restrict s, size_t n, const char *restrict format, va_list args);

static char out[200];
static unsigned nout;

static void flush(void)
{
    out[nout] = '\0';
    if (nout)
        acpid_say_acpica(out);
    nout = 0;
}

void AcpiOsVprintf(const char *format, va_list args)
{
    char piece[256];
    int n = vsnprintf(piece, sizeof piece, format, args);

    if (n > (int)sizeof piece - 1)
        n = sizeof piece - 1;
    for (int i = 0; i < n; i++) {
        if (piece[i] == '\n') {
            flush();
        } else if (nout < sizeof out - 1) {
            out[nout++] = piece[i];
        }
    }
}

void ACPI_INTERNAL_VAR_XFACE AcpiOsPrintf(const char *format, ...)
{
    va_list args;

    va_start(args, format);
    AcpiOsVprintf(format, args);
    va_end(args);
}

void AcpiOsRedirectOutput(void *)
{
}
