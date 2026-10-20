/*
 * arch/aarch64/efi.c -- the EFI stub: the kernel image as a UEFI application
 *
 * UEFI loads the image as a PE/COFF application (entry.S's header) and
 * calls efi_main(), at EL2 or EL1 with its MMU on and memory mapped 1:1.
 * The stub copies the image to a 2 MiB boundary, collects what the
 * kernel needs into a boot-information block (kernel/bootinfo.h), leaves
 * boot services and enters the copy through entry.S as any loader would,
 * MMU off.
 *
 * It runs before the kernel relocates itself, at whatever address UEFI
 * chose, so it must reach nothing through an absolute address: no
 * globals, no pointers in initialized data. Only PC-relative addressing,
 * which is what the compiler makes of everything else. The build checks
 * this file's object for absolute relocations (mk/arch-aarch64.mk).
 */
#include "aarch64.h"
#include "bootinfo.h"
#include "efi.h"
#include "kstring.h"

constexpr uint64_t MIB2 = 1ULL << 21;
constexpr uint64_t PAGES_PER_2MIB = MIB2 / EFI_PAGE_SIZE;
constexpr uint64_t DIRECT_MAP_TOP = 512ULL << 30;   /* entry.S's boot tables */

extern char _kernel_start[], _edata[], __bss_end[];  /* link.ld */

/* entry.S: write the image back and out of the caches, turn the MMU off
 * and enter it at its start. */
[[noreturn]] void efi_enter_kernel(uint64_t info, uint64_t image, uint64_t size);

efi_status efi_main(efi_handle image, struct efi_system_table *st);

/* --- Printing through UEFI's console. --- */

static void say(struct efi_system_table *st, const char *s)
{
    efi_char16 buf[128];
    size_t n = 0;

    while (*s) {
        if (*s == '\n' && n < 126)
            buf[n++] = '\r';
        buf[n++] = (efi_char16)(unsigned char)*s++;
        if (n >= 126 || !*s) {
            buf[n] = 0;
            st->con_out->output_string(st->con_out, buf);
            n = 0;
        }
    }
}

static void say_hex(struct efi_system_table *st, uint64_t v)
{
    char buf[19] = "0x";

    for (int i = 0; i < 16; i++)
        buf[2 + i] = "0123456789abcdef"[(v >> (60 - 4 * i)) & 0xf];
    buf[18] = 0;
    say(st, buf);
}

static efi_status fail(struct efi_system_table *st, const char *what, efi_status status)
{
    say(st, "planar: EFI stub: ");
    say(st, what);
    say(st, ", status ");
    say_hex(st, status);
    say(st, "\n");
    return status ? status : EFI_LOAD_ERROR;
}

/* --- What the kernel reads before its caches are on. --- */

static void dcache_clean(uint64_t start, uint64_t size)
{
    uint64_t line = 4ULL << ((sysreg_read(ctr_el0) >> 16) & 0xf);   /* DminLine */

    for (uint64_t a = start & ~(line - 1); a < start + size; a += line)
        __asm__ volatile("dc cvac, %0" : : "r"(a) : "memory");
    __asm__ volatile("dsb sy" : : : "memory");
}

static bool guid_eq(const struct efi_guid *a, const struct efi_guid *b)
{
    return memcmp(a, b, sizeof *a) == 0;
}

static uint32_t be32(const void *p)
{
    const uint8_t *b = p;

    return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
}

static void find_tables(struct efi_system_table *st, struct boot_info *bi)
{
    const struct efi_guid dtb = EFI_DTB_TABLE_GUID, acpi = EFI_ACPI_20_TABLE_GUID;

    for (uint64_t i = 0; i < st->tables; i++) {
        if (guid_eq(&st->table[i].guid, &dtb))
            bi->dtb = (uint64_t)(uintptr_t)st->table[i].table;
        else if (guid_eq(&st->table[i].guid, &acpi))
            bi->rsdp = (uint64_t)(uintptr_t)st->table[i].table;
    }
}

static void find_framebuffer(struct efi_system_table *st, struct boot_info *bi)
{
    const struct efi_guid guid = EFI_GOP_GUID;
    struct efi_gop *gop;

    if (st->boot_services->locate_protocol(&guid, nullptr, (void **)&gop) != EFI_SUCCESS ||
        !gop->mode || !gop->mode->info)
        return;
    bi->fb.base = gop->mode->frame_buffer_base;
    bi->fb.size = gop->mode->frame_buffer_size;
    bi->fb.width = gop->mode->info->width;
    bi->fb.height = gop->mode->info->height;
    bi->fb.stride = gop->mode->info->pixels_per_line;
    bi->fb.format = gop->mode->info->pixel_format;
}

/* The load options, UCS-2, as ASCII: what the boot entry or the shell
 * passed, which the kernel reads as its arguments. */
static void copy_cmdline(const struct efi_loaded_image *li, struct boot_info *bi)
{
    const efi_char16 *opt = li->load_options;
    size_t n = li->load_options_size / sizeof *opt, out = 0;

    for (size_t i = 0; opt && i < n && opt[i] && out < BOOT_INFO_CMDLINE - 1; i++)
        bi->cmdline[out++] = opt[i] < 0x80 && opt[i] >= 0x20 ? (char)opt[i] : ' ';
    bi->cmdline[out] = 0;
}

/* Pages for the image at a 2 MiB boundary, below the boot tables' reach:
 * allocate 2 MiB more than needed and give back both ends. */
static efi_status allocate_image(struct efi_boot_services *bs, uint64_t pages, uint64_t *at)
{
    uint64_t base = DIRECT_MAP_TOP - 1, aligned, head, tail;
    efi_status s = bs->allocate_pages(EFI_ALLOCATE_MAX_ADDRESS, EFI_LOADER_DATA,
                                      pages + PAGES_PER_2MIB, &base);

    if (s != EFI_SUCCESS)
        return s;
    aligned = (base + MIB2 - 1) & ~(MIB2 - 1);
    head = (aligned - base) / EFI_PAGE_SIZE;
    tail = PAGES_PER_2MIB - head;
    if (head)
        bs->free_pages(base, head);
    if (tail)
        bs->free_pages(aligned + pages * EFI_PAGE_SIZE, tail);
    *at = aligned;
    return EFI_SUCCESS;
}

efi_status efi_main(efi_handle image, struct efi_system_table *st)
{
    const struct efi_guid loaded_image = EFI_LOADED_IMAGE_GUID;
    struct efi_boot_services *bs = st->boot_services;
    uint64_t image_size = (uint64_t)(__bss_end - _kernel_start);
    uint64_t file_size = (uint64_t)(_edata - _kernel_start);
    uint64_t image_pages = (image_size + EFI_PAGE_SIZE - 1) / EFI_PAGE_SIZE;
    uint64_t kernel, info_pa, map_size = 0, map_cap, key, desc_size, info_pages;
    uint32_t desc_version;
    struct efi_loaded_image *li;
    struct boot_info *bi;
    efi_status s;

    say(st, (sysreg_read(CurrentEL) >> 2 & 3) == 2 ? "planar: EFI stub at EL2\n"
                                                   : "planar: EFI stub at EL1\n");
    if ((s = bs->handle_protocol(image, &loaded_image, (void **)&li)) != EFI_SUCCESS)
        return fail(st, "no loaded-image protocol", s);
    if ((s = allocate_image(bs, image_pages, &kernel)) != EFI_SUCCESS)
        return fail(st, "no memory for the kernel", s);

    /* The block, with room after it for the memory map, which grows by a
     * descriptor or two when this allocation splits a range. */
    s = bs->get_memory_map(&map_size, nullptr, &key, &desc_size, &desc_version);
    if (s != EFI_BUFFER_TOO_SMALL)
        return fail(st, "no memory map", s);
    map_cap = map_size + 16 * desc_size;
    info_pages = (sizeof *bi + map_cap + EFI_PAGE_SIZE - 1) / EFI_PAGE_SIZE;
    info_pa = DIRECT_MAP_TOP - 1;
    s = bs->allocate_pages(EFI_ALLOCATE_MAX_ADDRESS, EFI_LOADER_DATA, info_pages, &info_pa);
    if (s != EFI_SUCCESS)
        return fail(st, "no memory for the boot information", s);
    bi = (struct boot_info *)(uintptr_t)info_pa;
    memset(bi, 0, sizeof *bi);
    bi->magic = BOOT_INFO_MAGIC;
    bi->mmap = info_pa + sizeof *bi;
    find_tables(st, bi);
    find_framebuffer(st, bi);
    copy_cmdline(li, bi);

    memcpy((void *)(uintptr_t)kernel, _kernel_start, file_size);

    say(st, "planar: kernel at ");
    say_hex(st, kernel);
    say(st, bi->dtb ? ", device tree" : "");
    say(st, bi->rsdp ? ", ACPI" : "");
    say(st, "; leaving boot services\n");

    /* The key changes with every allocation, the console's included, so
     * a failed exit takes the map again, and nothing else may come
     * between the two. */
    for (int tries = 0;; tries++) {
        map_size = map_cap;
        s = bs->get_memory_map(&map_size, (struct efi_memory_descriptor *)(uintptr_t)bi->mmap,
                               &key, &desc_size, &desc_version);
        if (s != EFI_SUCCESS)
            return fail(st, "no memory map", s);
        if ((s = bs->exit_boot_services(image, key)) == EFI_SUCCESS)
            break;
        if (tries == 3)
            return fail(st, "ExitBootServices failed", s);
    }
    bi->mmap_size = map_size;
    bi->mmap_desc_size = desc_size;
    bi->mmap_desc_version = desc_version;

    dcache_clean(info_pa, sizeof *bi + map_size);
    if (bi->dtb)
        dcache_clean(bi->dtb, be32((const void *)(uintptr_t)(bi->dtb + 4)));
    efi_enter_kernel(info_pa, kernel, image_size);
}
