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
 * So may it to memory others lent the driver (its clients' buffers):
 * every Area the driver mapped but didn't own is held too.
 * The lease is then stopping. Its owner stops the device (it may map the
 * registers itself now) and calls RevokeLease, which frees the
 * quarantined Areas and leaves the lease ready to grant again.
 */
#ifndef PLANAR_LEASE_H
#define PLANAR_LEASE_H

#include <stdint.h>

#include <planar/exec.h>

#define LEASE_IRQS      32          /* eight virtio-mmio slots in a page, and room */

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
    uint64_t li_LentPages;          /* others' memory its dead grantee mapped, held as well */
    uint64_t li_DmaLimit;           /* its DMA Areas end below this; 0: anywhere */
    uint32_t li_Shares;             /* pages of it the grantee shares with peers */
    uint32_t li_Call;               /* the firmware function it may call (SetLeaseCall); 0: none */
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
 * Where the device can reach memory (v5-plan step 1). Under the identity
 * layout an Area's address is its DMA address only where the device's
 * bus doesn't move memory: the K1's GPU and display see memory above
 * 2 GiB at another address (its dram_range1). The owner, while the
 * lease is idle, says where that stops: every DMA Area of the lease
 * then ends at or below 'limit' (a page multiple; 0: anywhere). hwdb
 * takes it from the device's dma-ranges.
 */
int64_t LimitLeaseDma(Handle lease, uint64_t limit);

/*
 * A page of a lease for a peer (v5-plan step 1, §5.1): the grantee maps
 * 'size' bytes of its device's registers at 'address' (whole pages,
 * inside the lease) into the process of 'task', which must be connected
 * to it, as device memory, readable and writable. A CSF doorbell is the
 * case: the app rings its queue's own. The peer gets no lines and no
 * DMA. UnshareLease takes the pages at 'address' back; they go anyway
 * when the grantee's grant does (its death), or the peer dies. A peer
 * holds one share of a lease at a time per address (ERR_EXISTS);
 * LEASE_SHARES at most per lease (ERR_FULL).
 */
#define LEASE_SHARES    32

int64_t ShareLease(Handle lease, uint64_t address, uint64_t size, Handle task);
int64_t UnshareLease(Handle lease, uint64_t address, Handle task);

/*
 * A firmware call that goes with a device (v5-plan step 2): the CIX's
 * SCMI power domains are TF-A's, reached by SMC 0xc2000001. The owner,
 * while the lease is idle, names the one function ID its grantee may
 * call (0: none); hwdb takes it from a board quirk. CallLease makes it
 * by the SMC Calling Convention, through the conduit PSCI uses, with
 * x[1..3] as its arguments; x[0..3] are what came back. ERR_RIGHTS if
 * the lease has no function, ERR_NOSYS where there's no such conduit
 * (RISC-V). What the firmware answers is in x[0], not the return.
 */
int64_t SetLeaseCall(Handle lease, uint32_t fid);
int64_t CallLease(Handle lease, uint64_t x[4]);

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
    /* The framebuffer UEFI's GOP left set up (step 7), or base 0. It's
     * memory, but the one memory a lease may cover: exactly this. */
    uint64_t fi_FbBase, fi_FbSize;
    uint32_t fi_FbWidth, fi_FbHeight;
    uint32_t fi_FbStride;           /* pixels a line */
    uint32_t fi_FbFormat;           /* GOP's: 0 RGBX, 1 BGRX (8 bits each) */
};

int64_t MapFirmware(Handle root, struct FirmwareInfo *info);

/* The same tables, mapped into a process the caller created (acpid,
 * which runs the AML in them): the root's holder grants them. */
int64_t GrantFirmware(Handle root, Handle process);

/*
 * MSIs (v4-plan step 5), for the device root's holder: 'count' interrupt
 * sources for the device whose writes carry MSI device ID 'device' (a
 * PCI requester ID, as the msi-map or IORT translates it), its events 0
 * to count - 1, which a lease may then carry as lines. Each source is
 * edge-triggered: a driver's AckWaitIrq has nothing to unmask. A device
 * gets its sources once (ERR_EXISTS after); ERR_NOTFOUND if the
 * interrupt controller has no MSIs at all (a PLIC), ERR_FULL if it can't
 * give these (no ITS in use, or none left).
 */
/*
 * The serial console's input (step 7): the root's holder gives it to one
 * process (0: itself), which then reads what's been typed, up to 64 bytes
 * a call, without waiting: the bytes read, 0 if none, or ERR_RIGHTS if it
 * wasn't given the console. Polled; the kernel keeps writing its log.
 */
int64_t GrantConsole(Handle root, Handle process);
/* The root, to a process this one made, held: its first task gets the
 * handle as its second argument (init's hwdb, V6 step 7). */
int64_t GrantRoot(Handle root, Handle process);
/* Reset the board: a watchdog or PMIC, else PSCI or SBI. Returns only
 * if none can (V6 step 7). */
int64_t ColdReboot(Handle root);
/* The kernel log's last 'len' bytes at most (4096), oldest first, into
 * 'buf': what was shown on the console, and what's still to be, every
 * process's lines among the kernel's. The bytes copied (V6 step 8). */
int64_t ReadLog(Handle root, char *buf, uint32_t len);
/* End the process at the other end of a connection that 'process', one
 * the caller made, holds as 'conn' (V8 step 4: intuition's client not
 * answering, which intuition asks hwdb to end). ERR_NOTFOUND if the
 * other end has gone; ERR_RIGHTS if it's the caller or 'process'. */
int64_t KillPeer(Handle root, Handle process, Handle conn);
int64_t ReadConsole(char *buf, uint32_t len);

struct MsiInfo {
    uint64_t mi_Doorbell;           /* where the device writes an event's number */
    uint32_t mi_First;              /* the source event 0 raises; then 1, ... */
    uint32_t mi_Count;
};

int64_t AllocMsi(Handle root, uint32_t device, uint32_t count, struct MsiInfo *info);

#endif /* PLANAR_LEASE_H */
