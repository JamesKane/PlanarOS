/*
 * planar/lease.h -- device leases: the calls a driver and hwdb make
 *
 * docs/v4-plan.md step 1, §2.2, §11.4. A lease is a device's registers
 * (whole pages of MMIO) and up to LEASE_IRQS interrupt lines. Only the
 * holder of the device root, a handle the kernel gives the first server
 * (hwdb), can create one, and never over memory, over the devices the
 * kernel drives itself, or over another lease. Its creator owns it and
 * grants it to one process at a time: the driver, which maps the
 * registers at their physical address, binds the lines to signals, and
 * allocates the device's DMA memory against it.
 *
 * When the driver dies, its lines are masked at once, and the DMA Areas
 * it hadn't freed are quarantined: the device may still write to them.
 * The lease is then stopping. Its owner stops the device (it may map the
 * registers itself now) and calls RevokeLease, which frees the
 * quarantined Areas and leaves the lease ready to grant again.
 */
#ifndef PLANAR_LEASE_H
#define PLANAR_LEASE_H

#include <stdint.h>

#include <planar/exec.h>

#define LEASE_IRQS      4

/* LeaseInfo.li_State */
#define LEASES_IDLE     1           /* not granted */
#define LEASES_GRANTED  2
#define LEASES_STOPPING 3           /* its grantee died; RevokeLease when stopped */

struct LeaseInfo {
    uint64_t li_Base;               /* physical, and where it's mapped */
    uint64_t li_Size;
    uint32_t li_State;
    uint32_t li_Irqs;
    uint64_t li_DmaPages;           /* in live DMA Areas */
    uint64_t li_QuarantinedPages;   /* waiting for RevokeLease */
};

int64_t CreateLease(Handle root, uint64_t base, uint64_t size, const uint32_t *irqs,
                    uint32_t nirqs);
int64_t GrantLease(Handle lease, Handle process);
int64_t RevokeLease(Handle lease);
int64_t QueryLease(Handle lease, struct LeaseInfo *info);

/* The grantee's calls (the owner may map a lease nobody holds). */
int64_t MapLease(Handle lease, void **address);
int64_t BindIrq(Handle lease, uint32_t line, int64_t sigbit);
uint64_t AckWaitIrq(Handle lease, uint32_t line, uint64_t mask);
int64_t AllocDmaArea(Handle lease, uint64_t size, uint32_t flags, void **address);

/*
 * The firmware's description, for the device root's holder: the DTB, or
 * the ACPI tables (the RSDP, the XSDT, every table it lists, the DSDT
 * and FACS), mapped read-only at their physical addresses. The kernel
 * read one of them; an address is 0 for the other.
 */
struct FirmwareInfo {
    uint64_t fi_Dtb;
    uint64_t fi_DtbSize;
    uint64_t fi_Rsdp;
};

int64_t MapFirmware(Handle root, struct FirmwareInfo *info);

#endif /* PLANAR_LEASE_H */
