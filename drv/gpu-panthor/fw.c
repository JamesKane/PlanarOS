/*
 * drv/gpu-panthor/fw.c -- the CSF's firmware: loaded, booted and its
 * global interface set up (Linux's panthor_fw.c, panthor_fw_init)
 *
 * The firmware is mali_csffw.bin, arch 12.8's, which isn't in the
 * repository: the boot tools copy it from the NAS onto the boot volume
 * (DEVS/, by its SHA-256), and it's read through dos, as programs are,
 * once dos is there: from the loop, every half second until then.
 *
 * Its file is a header and entries. An interface entry is a section of
 * the MCU's memory: its addresses there (fixed, so the MCU's address
 * space isn't identity), what's copied into it from the file and
 * whether the rest is zeroed. Protected-mode sections are left out, as
 * panthor leaves them. The shared section, at 0x04000000, holds the
 * interfaces: the global one at its start, then each group's and each
 * of their streams', whose input and output pages are in it too.
 *
 * Booting: the L2 on, coherency ACE-Lite (the CIX patches), the MCU's
 * address space (0) enabled, then MCU_CONTROL to auto, and the job
 * line's global bit says it's up. Then the global interface's timers,
 * cores and interrupts, as panthor sets them, rung by doorbell 0.
 */
#include <stddef.h>

#include <planar/dos.h>
#include <planar/lease.h>
#include <planar/syscalls.h>

#include "panthor.h"

constexpr int64_t SIG_DOS = 27;
constexpr uint32_t FW_MAGIC = 0xc3f13a6e;
constexpr uint64_t FW_MAX = 4u << 20;
static const char FW_PATH[] = "SYS:EFI/planar/DEVS/mali_csffw.bin";

enum : uint32_t {               /* an interface entry's flags */
    IF_RD = 1u << 0, IF_WR = 1u << 1, IF_EX = 1u << 2, IF_PROT = 1u << 5, IF_SHARED = 1u << 30,
    IF_ZERO = 1u << 31,
};

/* Timers, in units of 1024 ticks of the system timer, which runs at
 * 1 GHz on the CIX (its README): panthor's 10 ms power-off and 800 µs
 * idle hysteresis; its progress timeout, 2.5 Gi GPU cycles, in 1024s. */
constexpr uint32_t POWEROFF_TIMER = 10000ull * 1000 / 1024;
constexpr uint32_t IDLE_TIMER = 800ull * 1000 / 1024;
constexpr uint32_t PROGRESS_TIMER = (5ull * 500 * 1024 * 1024) >> 10;

void *fw_cpu(uint32_t mcu_va)
{
    if (mcu_va < CSF_SHARED_VA || mcu_va - CSF_SHARED_VA >= gpu.shared_size)
        return nullptr;
    return (uint8_t *)gpu.shared + (mcu_va - CSF_SHARED_VA);
}

/* The whole file, into a new Area (*area): its size, or an error. */
static int64_t read_file(void **at, int64_t *area)
{
    struct Dos d;
    struct DosEntry e;
    int64_t f, n;

    if (OpenDos(&d, SIG_DOS) != 0)
        return ERR_NOTFOUND;            /* dos isn't there yet */
    if ((f = OpenFile(&d, FW_PATH, 0, &e)) < 0) {
        CloseDos(&d);
        return ERR_NOTFOUND;
    }
    if (e.de_Size < 64 || e.de_Size > FW_MAX || (*area = AllocArea(e.de_Size, 0, at)) < 0) {
        CloseDos(&d);
        return ERR_BADARG;
    }
    for (uint64_t off = 0; off < e.de_Size; off += (uint64_t)n) {
        uint32_t len = e.de_Size - off < DOS_MAX_IO ? (uint32_t)(e.de_Size - off) : DOS_MAX_IO;

        if ((n = ReadFile(&d, (uint32_t)f, off, (uint8_t *)*at + off, len)) <= 0) {
            FreeArea(*area);
            CloseDos(&d);
            return n < 0 ? n : ERR_BADARG;
        }
    }
    CloseDos(&d);
    return (int64_t)e.de_Size;
}

static uint32_t word(const uint8_t *p)
{
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* One interface entry: its section allocated, filled and mapped. */
static bool section(struct drv *d, const uint8_t *img, uint32_t size, const uint8_t *e)
{
    uint32_t flags = word(e), va = word(e + 4), va_end = word(e + 8);
    uint32_t data = word(e + 12), data_end = word(e + 16);
    void *at;

    if (va_end < va || data_end < data || data_end > size || (va | va_end) & (PAGE - 1) ||
        data_end - data > va_end - va) {
        glog("the firmware has a bad section");
        return false;
    }
    if (flags & IF_PROT || va == va_end)
        return true;                    /* protected mode's: not used */
    if (AllocDmaArea(drv_lease(d), va_end - va, AREAF_NC, &at) < 0) {
        glog("no memory for the firmware");
        return false;
    }
    memcpy(at, img + data, data_end - data);    /* the rest is zero, as Areas are */
    if (!vm_map(&gpu.mcu, va, (uint64_t)at, va_end - va, flags & IF_WR, flags & IF_EX)) {
        glog("the firmware's section wouldn't map");
        return false;
    }
    if (va == CSF_SHARED_VA) {
        if (!(flags & IF_SHARED))
            return false;
        gpu.shared = at;
        gpu.shared_size = va_end - va;
    }
    return true;
}

static bool load(struct drv *d, const uint8_t *img, uint32_t size)
{
    uint32_t hsize;

    if (word(img) != FW_MAGIC || img[5] != 0 || (hsize = word(img + 16)) > size) {
        glog("mali_csffw.bin isn't a CSF firmware this driver knows");
        return false;
    }
    for (uint32_t off = 20; off < hsize;) {
        uint32_t ehdr = word(img + off), esize = ehdr >> 8 & 0xff;

        if (esize < 4 || esize % 4 || off + esize > hsize) {
            glog("the firmware's entries are corrupt");
            return false;
        }
        if ((ehdr & 0xff) == 0 && (esize < 24 || !section(d, img, size, img + off + 4)))
            return false;
        /* The rest (config, trace buffers, metadata) are optional. */
        off += esize;
    }
    if (!gpu.shared) {
        glog("the firmware has no shared section");
        return false;
    }
    return true;
}

static bool l2_on(void)
{
    uint32_t prot = COHERENCY_ACE_LITE;

    if (!(gr(GPU_COHERENCY_FEATURES) & 1u << COHERENCY_ACE_LITE)) {
        glog("the GPU can't do ACE-Lite");
        return false;
    }
    if (gr(GPU_COHERENCY_FEATURES) & COHERENCY_SHAREABLE_CACHE)
        prot |= COHERENCY_SHAREABLE_CACHE;
    gw(GPU_COHERENCY_PROTOCOL, prot);
    gw64(L2_PWRON, 1);
    for (uint64_t until = CurrentTime() + 20 * MS; !(gr(L2_READY) & 1) || gr(L2_PWRTRANS) & 1;) {
        if (CurrentTime() > until) {
            glog("the L2 wouldn't power on");
            return false;
        }
        nap(1);
    }
    return true;
}

static bool mcu_start(void)
{
    gw(JOB_INT_CLEAR, ~0u);
    gw(JOB_INT_MASK, ~0u);
    gw(MCU_CONTROL, MCU_AUTO);
    for (uint64_t until = CurrentTime() + 1000 * MS; !(gr(JOB_INT_RAWSTAT) & JOB_INT_GLOBAL);) {
        if (CurrentTime() > until) {
            say("the MCU didn't boot: its status ", gr(MCU_STATUS), 10, "");
            return false;
        }
        nap(1);
    }
    gw(JOB_INT_CLEAR, JOB_INT_GLOBAL);
    return true;
}

/* --- The global interface. --- */

bool fw_wait(volatile uint32_t *req, volatile uint32_t *ack, uint32_t mask, uint64_t ms)
{
    for (uint64_t until = CurrentTime() + ms * MS; (*ack & mask) != (*req & mask);) {
        if (CurrentTime() > until)
            return false;
        nap(1);
    }
    return true;
}

void fw_glb_req(uint32_t value, uint32_t mask)
{
    gpu.gin->req = (gpu.gin->req & ~mask) | (value & mask);
}

void fw_glb_toggle(uint32_t mask)
{
    gpu.gin->req = ((gpu.gout->ack ^ mask) & mask) | (gpu.gin->req & ~mask);
}

void fw_ring(unsigned doorbell)
{
    drv_io_barrier();
    gw(CSF_DOORBELL0 + 0x10000 * doorbell, 1);
}

/* The firmware's watchdog: panthor pings it every 12 s, and resets the
 * GPU if it doesn't answer in 100 ms. */
void fw_ping(void)
{
    fw_glb_toggle(GLB_PING);
    fw_ring(0);
    if (!fw_wait(&gpu.gin->req, &gpu.gout->ack, GLB_PING, 100)) {
        glog("the firmware didn't answer a ping: given up");
        gpu.broken = true;
    }
}

static bool interfaces(void)
{
    const volatile struct glb_control *g = gpu.shared;

    if (!g->version) {
        glog("the firmware gave no interface version: it didn't boot");
        return false;
    }
    gpu.glb = (volatile struct glb_control *)g;
    gpu.gin = fw_cpu(g->input_va);
    gpu.gout = fw_cpu(g->output_va);
    if (!gpu.gin || !gpu.gout || g->group_num < 3 || g->group_num > 31) {
        glog("the firmware's global interface is out of its shared section");
        return false;
    }
    gpu.nslots = g->group_num < GPU_SLOTS ? g->group_num : GPU_SLOTS;
    /* One address space per slot, after the MCU's. */
    if (gpu.nslots > (unsigned)__builtin_popcount(gpu.as_present) - 1)
        gpu.nslots = (unsigned)__builtin_popcount(gpu.as_present) - 1;
    {
        const volatile struct cs_control *cs = (const volatile struct cs_control *)(
            (uint8_t *)gpu.shared + CSF_GROUP_CONTROL + CSF_STREAM_CONTROL);

        gpu.cs_regs = (cs->features & 0xff) + 1;
    }

    gpu.gin->core_en_mask = gpu.cores;
    gpu.gin->poweroff_timer = POWEROFF_TIMER;
    gpu.gin->progress_timer = PROGRESS_TIMER;
    gpu.gin->idle_timer = IDLE_TIMER;
    gpu.gin->ack_irq_mask = GLB_CFG_ALLOC_EN | GLB_PING | GLB_CFG_PROGRESS_TIMER |
                            GLB_CFG_POWEROFF_TIMER | GLB_IDLE_EN | GLB_IDLE;
    fw_glb_req(GLB_IDLE_EN, GLB_IDLE_EN);
    fw_glb_toggle(GLB_CFG_ALLOC_EN | GLB_CFG_POWEROFF_TIMER | GLB_CFG_PROGRESS_TIMER);
    fw_ring(0);
    if (!fw_wait(&gpu.gin->req, &gpu.gout->ack,
                 GLB_CFG_ALLOC_EN | GLB_CFG_POWEROFF_TIMER | GLB_CFG_PROGRESS_TIMER | GLB_IDLE_EN,
                 100)) {
        glog("the firmware didn't take its configuration");
        return false;
    }
    return true;
}

int fw_boot(struct drv *d)
{
    void *img;
    int64_t area, size = read_file(&img, &area);
    bool ok;

    if (size < 0)
        return (int)size;
    ok = l2_on() && vm_new(&gpu.mcu) && load(d, img, (uint32_t)size);
    FreeArea(area);
    if (!ok || !vm_enable(&gpu.mcu, 0) || !mcu_start() || !interfaces())
        return ERR_BADARG;
    gpu.up = true;
    return 0;
}
