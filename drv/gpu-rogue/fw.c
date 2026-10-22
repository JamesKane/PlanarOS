/*
 * drv/gpu-rogue/fw.c -- the MIPS firmware: loaded, mapped and started,
 * and the kernel and firmware CCBs (Linux's pvr_fw.c, pvr_fw_mips.c,
 * pvr_vm_mips.c, pvr_fw_startstop.c and pvr_ccb.c)
 *
 * The firmware is mainline's open one for this core,
 * rogue_36.29.52.182_v1.fw, which isn't in the repository: make sdcard
 * copies it from the NAS onto the card (DEVS/rogue.fw, by its SHA-256),
 * and it's read through dos, as programs are, once dos is there. It's a
 * MIPS ELF with, in its last 4 KiB, a header and a table of sections:
 * where each goes in the code or the data object, and where the MIPS
 * sees it. Its features and quirks are in the file too (the BRN 63553
 * remap below is one); this driver knows the one core, and checks the
 * file is that core's.
 *
 * The MIPS sees a 16 MiB heap at 0xc0000000 through a single-level page
 * table, 4 pages the host writes, and three fixed 4 KiB windows its
 * wrapper remaps: boot code, boot data and exceptions. Every address
 * the GPU is given is a bus address (bus_of): the K1's GPU sees memory
 * above 4 GiB 2 GiB lower. The firmware's own memory is from the lease,
 * below 2 GiB (hwdb's DMA limit), so its bus addresses are its own.
 * None of it is cached: the K1's GPU doesn't snoop.
 *
 * The code object is at the heap's start and the data object at its
 * private data's address; the config objects (sysinit, osinit, the
 * connection's) at mainline's fixed offsets; everything else, and what
 * clients lend, anywhere in between.
 */
#include <stddef.h>

#include <planar/dos.h>
#include <planar/lease.h>
#include <planar/syscalls.h>

#include "rogue.h"

void *memset(void *dst, int c, size_t n);
void *memcpy(void *restrict dst, const void *restrict src, size_t n);

constexpr int64_t SIG_DOS = 27;
constexpr uint64_t FW_MAX = 1u << 20;
static const char FW_PATH[] = "SYS:EFI/planar/DEVS/rogue.fw";

/* pvr_fw_info.h: the header and its layout entries. */
enum : uint32_t {
    H_VERSION = 0, H_LEN = 4, H_ENTRIES = 8, H_ENTRY_SIZE = 12, H_BVNC = 16, H_PAGE = 24,
    H_FLAGS = 28, H_MAJOR = 32, H_BUILD = 36,
};
enum : uint32_t { E_ID = 0, E_TYPE = 4, E_BASE = 8, E_ALLOC = 16, E_OFFSET = 20 };
enum : uint32_t {
    S_CODE = 4, S_EXCEPTIONS = 5, S_BOOT_CODE = 6, S_PRIVATE_DATA = 7, S_BOOT_DATA = 8,
    S_STACK = 9,
};
enum : uint32_t { T_CODE = 1, T_DATA = 2 };

/* The firmware's heap: a byte a page, 1 if used. */
static uint8_t used[FW_HEAP_PAGES];

/* --- The heap. --- */

static uint64_t pa_of(const void *p)
{
    return (uint64_t)p;                 /* the identity layout */
}

static void pte(uint32_t off, uint64_t bus)
{
    gpu.pt[off / PAGE] = (uint32_t)((bus >> 6) & 0x3fffffc0) | MIPS_PTE_FLAGS;
    used[off / PAGE] = 1;
}

/* 'size' bytes at 'pa', mapped at heap offset 'off'. */
static bool map_at(uint32_t off, uint64_t pa, uint64_t size)
{
    for (uint64_t o = 0; o < size; o += PAGE) {
        uint64_t bus = bus_of(pa + o);

        if (!bus || bus >> 36)
            return false;
        pte(off + (uint32_t)o, bus);
    }
    return true;
}

uint32_t heap_map(uint64_t pa, uint64_t size)
{
    uint32_t n = (uint32_t)((size + PAGE - 1) / PAGE);

    for (uint32_t first = 0, run = 0, p = 0; p < FW_HEAP_MAIN / PAGE; p++) {
        if (used[p]) {
            run = 0;
            continue;
        }
        if (run++ == 0)
            first = p;
        if (run == n) {
            if (!map_at(first * PAGE, pa, size)) {
                heap_unmap(FW_HEAP | first * PAGE, (uint64_t)n * PAGE);
                return 0;
            }
            return FW_HEAP | first * PAGE;
        }
    }
    return 0;
}

void heap_unmap(uint32_t fw, uint64_t size)
{
    for (uint32_t p = (fw - FW_HEAP) / PAGE, n = 0; n < (size + PAGE - 1) / PAGE; n++, p++) {
        gpu.pt[p] = 0;
        used[p] = 0;
    }
}

static bool alloc(struct drv *d, uint32_t size, struct fwobj *o)
{
    void *at;

    size = (uint32_t)((size + PAGE - 1) & ~(PAGE - 1));
    if (AllocDmaArea(drv_lease(d), size, AREAF_NC, &at) < 0)
        return false;
    /* No trampoline (mainline has none): the MIPS's boot windows are at
     * these addresses, and a page there would be shadowed. */
    if (pa_of(at) < 0x1fc04000 && pa_of(at) + size > 0x1fc00000) {
        glog("firmware memory fell in the MIPS's boot windows");
        return false;
    }
    *o = (struct fwobj){ .cpu = at, .size = size };
    return true;
}

bool fw_obj(struct drv *d, uint32_t size, struct fwobj *o)
{
    if (!alloc(d, size, o))
        return false;
    if (!(o->fw = heap_map(pa_of(o->cpu), o->size))) {
        fw_obj_free(o);
        return false;
    }
    return true;
}

static bool fw_obj_at(struct drv *d, uint32_t size, uint32_t off, struct fwobj *o)
{
    if (!alloc(d, size, o) || !map_at(off, pa_of(o->cpu), o->size))
        return false;
    o->fw = FW_HEAP | off;
    return true;
}

void fw_obj_free(struct fwobj *o)
{
    struct AreaInfo ai;

    if (o->fw)
        heap_unmap(o->fw, o->size);
    if (o->cpu && QueryArea(o->cpu, &ai) == 0 && ai.ai_Area)
        FreeArea(ai.ai_Area);
    *o = (struct fwobj){ 0 };
}

/* --- The file. --- */

static int64_t read_file(void **at, int64_t *area)
{
    struct dos d;
    struct DosEntry e;
    int64_t f, n;

    if (dos_open(&d, SIG_DOS) != 0)
        return ERR_NOTFOUND;            /* dos isn't there yet */
    if ((f = dos_file(&d, FW_PATH, 0, &e)) < 0) {
        dos_close(&d);
        return ERR_NOTFOUND;
    }
    if (e.de_Size < 2 * PAGE || e.de_Size > FW_MAX || e.de_Size % PAGE ||
        (*area = AllocArea(e.de_Size, 0, at)) < 0) {
        dos_close(&d);
        return ERR_BADARG;
    }
    for (uint64_t off = 0; off < e.de_Size; off += (uint64_t)n) {
        uint32_t len = e.de_Size - off < DOS_MAX_IO ? (uint32_t)(e.de_Size - off) : DOS_MAX_IO;

        if ((n = dos_read(&d, (uint32_t)f, off, (uint8_t *)*at + off, len)) <= 0) {
            FreeArea(*area);
            dos_close(&d);
            return n < 0 ? n : ERR_BADARG;
        }
    }
    dos_close(&d);
    return (int64_t)e.de_Size;
}

static uint32_t le32(const uint8_t *p)
{
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t le64(const uint8_t *p)
{
    return le32(p) | (uint64_t)le32(p + 4) << 32;
}

struct layout {
    const uint8_t *entry[S_STACK + 1];  /* by section id */
    const uint8_t *all;
    uint32_t n;
    uint32_t code_size, data_size;
};

/* pvr_fw_validate: the header, and that it's this core's. */
static bool check(const uint8_t *img, uint32_t size, struct layout *l)
{
    const uint8_t *h = img + size - PAGE;

    if (le32(h + H_VERSION) != 3 || le32(h + H_LEN) != 48 || le32(h + H_ENTRY_SIZE) != 24 ||
        le32(h + H_ENTRIES) > 8 || !(le32(h + H_FLAGS) & 1) ||
        (le32(h + H_MAJOR) & 0xffff) != 1 || le32(h + H_PAGE) != PAGE) {
        glog("rogue.fw isn't mainline's open firmware, version 1");
        return false;
    }
    if (le64(h + H_BVNC) != BVNC) {
        say("rogue.fw is for another core: ", le64(h + H_BVNC), 16, "");
        return false;
    }
    *l = (struct layout){ .all = h + 48, .n = le32(h + H_ENTRIES) };
    for (uint32_t i = 0; i < l->n; i++) {
        const uint8_t *e = l->all + 24 * i;
        uint32_t id = le32(e + E_ID), end = le32(e + E_OFFSET) + le32(e + E_ALLOC);

        if (id <= S_STACK)
            l->entry[id] = e;
        if (le32(e + E_TYPE) == T_CODE && end > l->code_size)
            l->code_size = end;
        if (le32(e + E_TYPE) == T_DATA && end > l->data_size)
            l->data_size = end;
    }
    for (uint32_t id = S_CODE; id <= S_STACK; id++)
        if (!l->entry[id]) {
            glog("rogue.fw lacks one of the MIPS's sections");
            return false;
        }
    say("firmware build ", le32(h + H_BUILD), 10, "");
    return true;
}

/* The ELF's loadable segments, each into the section holding it
 * (pvr_fw_process_elf_command_stream). */
static bool load(const uint8_t *img, uint32_t size, const struct layout *l)
{
    uint32_t phoff = le32(img + 28), phnum = img[44] | (uint32_t)img[45] << 8;

    if (le32(img) != 0x464c457f || img[4] != 1 || phoff + 32 * phnum > size) {
        glog("rogue.fw's image isn't a 32-bit ELF");
        return false;
    }
    for (uint32_t i = 0; i < phnum; i++) {
        const uint8_t *ph = img + phoff + 32 * i;
        uint32_t off = le32(ph + 4), va = le32(ph + 8), filesz = le32(ph + 16), memsz = le32(ph + 20);
        bool placed = false;

        if (le32(ph) != 1)
            continue;                   /* not PT_LOAD */
        if (filesz > memsz || off + filesz > size)
            return false;
        for (uint32_t k = 0; k < l->n && !placed; k++) {
            const uint8_t *e = l->all + 24 * k;
            uint32_t base = le32(e + E_BASE), alloc = le32(e + E_ALLOC);
            uint8_t *dst;

            if (va < base || va - base + memsz > alloc)
                continue;
            dst = (le32(e + E_TYPE) == T_CODE ? gpu.code.cpu : gpu.data.cpu) +
                  le32(e + E_OFFSET) + (va - base);
            memcpy(dst, img + off, filesz);
            memset(dst + filesz, 0, memsz - filesz);
            placed = true;
        }
        if (!placed) {
            say("rogue.fw has a segment no section holds, at ", va, 16, "");
            return false;
        }
    }
    return true;
}

static uint64_t bus_in(const struct fwobj *o, const uint8_t *e)
{
    return bus_of(pa_of(o->cpu) + le32(e + E_OFFSET));
}

/* --- Its structures. --- */

static struct fwobj hwrinfo, sysdata, fault, gpu_util, runtime_cfg, trace, trace_buf[2], osdata;

static bool structures(struct drv *d)
{
    if (!fw_obj(d, CTL_SIZE, &gpu.kccb_ctl) || !fw_obj(d, KCCB_N * KCCB_CMD, &gpu.kccb) ||
        !fw_obj(d, KCCB_N * 4, &gpu.kccb_rtn) || !fw_obj(d, CTL_SIZE, &gpu.fwccb_ctl) ||
        !fw_obj(d, FWCCB_N * KCCB_CMD, &gpu.fwccb) || !fw_obj(d, 8, &gpu.sync) ||
        !fw_obj(d, HWRINFO_SIZE, &hwrinfo) || !fw_obj(d, SYSDATA_SIZE, &sysdata) ||
        !fw_obj(d, PAGE, &fault) || !fw_obj(d, GPU_UTIL_SIZE, &gpu_util) ||
        !fw_obj(d, RC_SIZE, &runtime_cfg) || !fw_obj(d, TB_SIZE, &trace) ||
        !fw_obj(d, TB_BUF_DWORDS * 4, &trace_buf[0]) ||
        !fw_obj(d, TB_BUF_DWORDS * 4, &trace_buf[1]) || !fw_obj(d, OD_SIZE, &osdata) ||
        !fw_obj_at(d, CONNECTION_SIZE, FW_CONNECTION, &gpu.connection) ||
        !fw_obj_at(d, OI_SIZE, FW_OSINIT, &gpu.osinit) ||
        !fw_obj_at(d, SI_SIZE, FW_SYSINIT, &gpu.sysinit)) {
        glog("no memory for the firmware's structures");
        return false;
    }

    /* The CCBs: offsets in commands, of 88 bytes. */
    put32(gpu.kccb_ctl.cpu, CTL_WRAP, KCCB_N - 1);
    put32(gpu.kccb_ctl.cpu, CTL_CMD_SIZE, KCCB_CMD);
    put32(gpu.fwccb_ctl.cpu, CTL_WRAP, FWCCB_N - 1);
    put32(gpu.fwccb_ctl.cpu, CTL_CMD_SIZE, KCCB_CMD);

    for (uint32_t i = 0; i < PAGE; i += 4)
        put32(fault.cpu, i, 0xdeadbee0);
    put32(runtime_cfg.cpu, RC_PM_LATENCY_PERSIST, 1);
    put32(runtime_cfg.cpu, RC_CLOCK, CORE_CLOCK);
    put32(runtime_cfg.cpu, RC_DUSTS, 1);            /* NUM_CLUSTERS */
    put32(trace.cpu, TB_LOG_TYPE, 0);               /* none */
    put32(trace.cpu, TB_DWORDS, TB_BUF_DWORDS);
    for (uint32_t t = 0; t < 2; t++)
        put32(trace.cpu, TB_SPACE + TB_SPACE_SIZE * t + TB_SPACE_BUF, trace_buf[t].fw);
    put32(osdata.cpu, OD_POWER_SYNC, gpu.sync.fw);
    /* sysdata's config flags stay 0: the SLC is 16 KiB, so DM overlap is on. */

    put32(gpu.osinit.cpu, OI_KCCB_CTL, gpu.kccb_ctl.fw);
    put32(gpu.osinit.cpu, OI_KCCB, gpu.kccb.fw);
    put32(gpu.osinit.cpu, OI_KCCB_RTN, gpu.kccb_rtn.fw);
    put32(gpu.osinit.cpu, OI_FWCCB_CTL, gpu.fwccb_ctl.fw);
    put32(gpu.osinit.cpu, OI_FWCCB, gpu.fwccb.fw);
    put32(gpu.osinit.cpu, OI_HWRINFO, hwrinfo.fw);
    put32(gpu.osinit.cpu, OI_OSDATA, osdata.fw);
    put32(gpu.osinit.cpu, OI_HW_BVNC, 3);           /* the firmware fills in the rest */
    put32(gpu.osinit.cpu, OI_FW_BVNC, 3);

    put64(gpu.sysinit.cpu, SI_FAULT_PHYS, bus_of(pa_of(fault.cpu)));
    put64(gpu.sysinit.cpu, SI_PDS_EXEC, 0xda00000000ull);
    put64(gpu.sysinit.cpu, SI_USC_EXEC, 0xe000000000ull);
    put32(gpu.sysinit.cpu, SI_RUNTIME_CFG, runtime_cfg.fw);
    put32(gpu.sysinit.cpu, SI_TRACE, trace.fw);
    put32(gpu.sysinit.cpu, SI_SYSDATA, sysdata.fw);
    put32(gpu.sysinit.cpu, SI_GPU_UTIL, gpu_util.fw);
    put32(gpu.sysinit.cpu, SI_CLOCK, CORE_CLOCK);
    put32(gpu.sysinit.cpu, SI_STARTED, 0);
    put32(gpu.sysinit.cpu, SI_MARKER, 1);
    return true;
}

/* --- Starting it (pvr_fw_start, pvr_mips_wrapper_init). --- */

static void delay_us(uint64_t us)
{
    for (uint64_t until = CurrentTime() + us * 1000; CurrentTime() < until;)
        ;
}

static void reset_fence(void)
{
    (void)gr64(CR_SOFT_RESET);
    (void)gr64(CR_SOFT_RESET2);
}

static void start(const struct layout *l)
{
    uint64_t boot_code = bus_in(&gpu.code, l->entry[S_BOOT_CODE]);
    uint64_t boot_data = bus_in(&gpu.data, l->entry[S_BOOT_DATA]);
    uint64_t exceptions = bus_in(&gpu.code, l->entry[S_EXCEPTIONS]);
    constexpr uint64_t ADDR = 0xfffffff000ull, SEG = 12;    /* 4 KiB windows */
    uint32_t misc;

    gw(CR_SYS_BUS_SECURE, 0);
    (void)gr(CR_SYS_BUS_SECURE);
    gw64(CR_SOFT_RESET, RESET_ALL);
    gw64(CR_SOFT_RESET2, RESET2_ALL);
    reset_fence();
    gw64(CR_SOFT_RESET, RESET_ALL ^ RESET_RASCALDUSTS);
    gw64(CR_SOFT_RESET2, 0);
    reset_fence();
    gw64(CR_SOFT_RESET, RESET_GARTEN);
    gw64(CR_SOFT_RESET2, 0);
    reset_fence();

    /* The SLC: PVR_HASH1 decoding, and the burst combiner bypassed (its
     * lines are 512 bits). */
    misc = gr(CR_SLC_CTRL_MISC);
    gw(CR_SLC_CTRL_MISC, (misc & 0x2) | 0x00200000 | 0x1);

    /* The wrapper: the registers' window for the MIPS, microMIPS from
     * reset; boot code, BRN 63553's remap of address 0 to it, boot data
     * and exceptions. */
    gw(CR_MIPS_WRAPPER_CONFIG, MIPS_REGS >> 16 | 0x10000);
    gw64(CR_MIPS_REMAP1_CONFIG1, MIPS_BOOT_CODE | 1);
    gw64(CR_MIPS_REMAP1_CONFIG2, (boot_code & ADDR) | SEG);
    gw64(CR_MIPS_REMAP5_CONFIG1, 0 | 1);
    gw64(CR_MIPS_REMAP5_CONFIG2, (boot_code & ADDR) | SEG);
    gw64(CR_MIPS_REMAP1_CONFIG1 + 0x10, MIPS_BOOT_DATA | 1);
    gw64(CR_MIPS_REMAP1_CONFIG2 + 0x10, (boot_data & ADDR) | SEG);
    gw64(CR_MIPS_REMAP1_CONFIG1 + 0x20, MIPS_EXCEPTIONS | 1);
    gw64(CR_MIPS_REMAP1_CONFIG2 + 0x20, (exceptions & ADDR) | SEG);
    gw64(CR_MTS_GARTEN_WRAPPER_CONFIG, 0);
    gw(CR_MIPS_DEBUG_CONFIG, 0);

    /* AXI: everything to the outer cache, non-snooping (rogue_axi_ace_list_init). */
    gw64(CR_AXI_ACE_LITE_CONFIGURATION, 0x8882af00);

    drv_io_barrier();
    delay_us(3);
    gw64(CR_SOFT_RESET, 0);                 /* the MIPS starts */
    (void)gr64(CR_SOFT_RESET);
    delay_us(3);
}

int fw_boot(struct drv *d)
{
    void *img;
    int64_t size, area;
    struct layout l;
    uint8_t *bd;
    void *pt;
    uint64_t until;

    if ((size = read_file(&img, &area)) < 0)
        return size == ERR_NOTFOUND ? ERR_NOTFOUND : (int)size;
    if (!check(img, (uint32_t)size, &l) ||
        AllocDmaArea(drv_lease(d), MIPS_PT_PAGES * PAGE, AREAF_NC, &pt) < 0 ||
        !alloc(d, l.code_size, &gpu.code) || !alloc(d, l.data_size, &gpu.data)) {
        FreeArea(area);
        return ERR_BADARG;
    }
    gpu.pt = pt;
    if (!load(img, (uint32_t)size, &l)) {
        FreeArea(area);
        return ERR_BADARG;
    }

    /* Code at the heap's start, data at its private data's address. */
    if (!map_at(0, pa_of(gpu.code.cpu), gpu.code.size) ||
        !map_at(le32(l.entry[S_PRIVATE_DATA] + E_BASE) - FW_HEAP, pa_of(gpu.data.cpu),
                gpu.data.size)) {
        glog("the firmware's code and data wouldn't map");
        FreeArea(area);
        return ERR_BADARG;
    }
    gpu.code.fw = FW_HEAP;
    gpu.data.fw = le32(l.entry[S_PRIVATE_DATA] + E_BASE);
    /* Everything below the data's end is theirs, or the firmware's own. */
    for (uint32_t p = 0; p < (gpu.data.fw - FW_HEAP + gpu.data.size) / PAGE; p++)
        used[p] = 1;
    for (uint32_t p = FW_HEAP_MAIN / PAGE; p < FW_HEAP_PAGES; p++)
        used[p] = 1;

    bd = gpu.data.cpu + le32(l.entry[S_BOOT_DATA] + E_OFFSET);
    put64(bd, BD_STACK, bus_in(&gpu.data, l.entry[S_STACK]));
    put64(bd, BD_REG_BASE, gpu.regs_pa);
    for (uint32_t i = 0; i < MIPS_PT_PAGES; i++)
        put64(bd, BD_PT + 8 * i, bus_of(pa_of(gpu.pt) + i * PAGE));
    put32(bd, BD_PT_LOG2, 12);
    put32(bd, BD_PT_PAGES, MIPS_PT_PAGES);

    if (!structures(d)) {
        FreeArea(area);
        return ERR_NOMEM;
    }
    drv_io_barrier();
    start(&l);
    FreeArea(area);                     /* the layout's entries are in it */

    until = CurrentTime() + 5000 * MS;
    while (!get32(gpu.sysinit.cpu, SI_STARTED)) {
        if (CurrentTime() > until) {
            say("the firmware didn't start: its boot stage ", get32(bd, BD_STAGE), 16, "");
            say("  MIPS exception status ", gr(CR_MIPS_EXCEPTION_STATUS), 16, "");
            say("  MIPS wrapper status ", gr(CR_MIPS_WRAPPER_STATUS), 16, "");
            gw64(CR_SOFT_RESET, RESET_ALL);
            return ERR_TIMEOUT;
        }
        nap(1);
    }
    gpu.up = true;
    say("firmware up: its checks say core ", get32(gpu.osinit.cpu, OI_HW_BVNC + 8), 16, "");
    return 0;
}

/* --- The kernel CCB (pvr_kccb_send_cmd). --- */

bool kccb_send(const uint32_t *cmd, unsigned words, uint32_t *slot)
{
    uint32_t w = get32(gpu.kccb_ctl.cpu, CTL_WRITE);
    uint64_t until = CurrentTime() + 100 * MS;
    uint8_t *at = gpu.kccb.cpu + w * KCCB_CMD;

    while (((w + 1) & (KCCB_N - 1)) == get32(gpu.kccb_ctl.cpu, CTL_READ))
        if (CurrentTime() > until) {
            glog("the kernel CCB stayed full");
            return false;
        }
    for (unsigned i = 0; i < KCCB_CMD / 4; i++)
        put32(at, 4 * i, i < words ? cmd[i] : 0);
    put32(gpu.kccb_rtn.cpu, 4 * w, 0);
    drv_io_barrier();
    put32(gpu.kccb_ctl.cpu, CTL_WRITE, (w + 1) & (KCCB_N - 1));
    drv_io_barrier();
    gw(CR_MTS_SCHEDULE, 0);             /* the GP DM, background, uncounted */
    drv_io_barrier();
    gpu.kccb_sent++;
    if (slot)
        *slot = w;
    return true;
}

bool kccb_wait(uint32_t slot, uint64_t ms, uint32_t *rtn)
{
    uint64_t until = CurrentTime() + ms * MS;
    uint32_t r;

    while (!((r = get32(gpu.kccb_rtn.cpu, 4 * slot)) & RTN_EXECUTED))
        if (CurrentTime() > until)
            return false;
    if (rtn)
        *rtn = r;
    return true;
}

/* A health check answers in no slot: it's seen as the firmware's count of
 * kernel CCB commands done going past it (mainline's watchdog). */
bool fw_health_check(void)
{
    uint32_t cmd[2] = { KCCB_HEALTH_CHECK, 0 }, slot;
    uint32_t done = get32(osdata.cpu, OD_KCCB_EXECUTED);
    uint64_t until = CurrentTime() + 1000 * MS;

    if (!kccb_send(cmd, 2, &slot))
        return false;
    while (get32(osdata.cpu, OD_KCCB_EXECUTED) == done)
        if (CurrentTime() > until) {
            glog("the firmware didn't answer its kernel CCB");
            fw_dump(slot);
            return false;
        }
    return true;
}

/* What the host can see of a firmware that didn't answer. */
void fw_dump(uint32_t slot)
{
    say("  kernel CCB written to ", get32(gpu.kccb_ctl.cpu, CTL_WRITE), 10, "");
    say("  kernel CCB read to ", get32(gpu.kccb_ctl.cpu, CTL_READ), 10, "");
    say("  its slot's return ", get32(gpu.kccb_rtn.cpu, 4 * slot), 16, "");
    say("  firmware CCB written to ", get32(gpu.fwccb_ctl.cpu, CTL_WRITE), 10, "");
    say("  commands done ", get32(osdata.cpu, OD_KCCB_EXECUTED), 10, "");
    say("  connection: firmware state ", get32(gpu.connection.cpu, 0), 16, "");
    say("  connection: host state ", get32(gpu.connection.cpu, 4), 16, "");
    say("  MIPS IRQ status ", gr(CR_MIPS_IRQ_STATUS), 16, "");
    say("  MIPS exception status ", gr(CR_MIPS_EXCEPTION_STATUS), 16, "");
    say("  MIPS wrapper status ", gr(CR_MIPS_WRAPPER_STATUS), 16, "");
    say("  MIPS unmapped remap ", gr64(0x0868), 16, "");
    say("  MTS interrupt context ", gr(0x0b98), 16, "");
    say("  MTS background context ", gr(0x0ba0), 16, "");
    say("  MTS schedule enable ", gr(0x0bc8), 16, "");
    say("  event status ", gr(0x0130), 16, "");
}

bool fw_mmu_flush(void)
{
    uint32_t cmd[5] = { KCCB_MMUCACHE, 0, MMUCACHE_ALL, gpu.sync.fw + 4, ++gpu.mmu_seq };
    uint32_t slot;

    return kccb_send(cmd, 5, &slot) && kccb_wait(slot, 1000, nullptr);
}

/* --- The firmware CCB: what it tells the host. --- */

void fw_events(void)
{
    uint32_t r;

    while ((r = get32(gpu.fwccb_ctl.cpu, CTL_READ)) != get32(gpu.fwccb_ctl.cpu, CTL_WRITE)) {
        const uint8_t *c = gpu.fwccb.cpu + r * KCCB_CMD;
        uint32_t type = get32(c, 0);

        if (type == FWCCB_CONTEXT_RESET) {
            uint32_t id = get32(c, 8), why = get32(c, 12);

            if (id < GPU_CLIENTS && gpu.clients[id].open) {
                gpu.clients[id].fault = why ? why : 1;
                gpu.clients[id].fault_addr = get32(c, 40) | (uint64_t)get32(c, 44) << 32;
                gpu.clients[id].faults++;
            }
            say("a context was reset: reason ", why, 10, "");
        } else if (type == FWCCB_REQUEST_GPU_RESTART) {
            glog("the firmware asked for a restart: given up");
            gpu.broken = true;
        } else {
            say("the firmware said ", type, 16, "");
        }
        put32(gpu.fwccb_ctl.cpu, CTL_READ, (r + 1) & (FWCCB_N - 1));
    }
}
