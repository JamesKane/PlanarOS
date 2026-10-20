/*
 * kernel/bootinfo.h -- what a loader hands the kernel besides a device tree
 *
 * Firmware that boots the kernel directly (QEMU's -kernel, U-Boot's
 * booti, OpenSBI) passes a device tree. The EFI stub passes one of these
 * instead, in the same register: what it learned from UEFI before
 * leaving it. The kernel tells the two apart by the first word.
 */
#ifndef KERNEL_BOOTINFO_H
#define KERNEL_BOOTINFO_H

#include <stdint.h>

constexpr uint64_t BOOT_INFO_MAGIC = 0x4f464e49544f4f42;   /* "BOOTINFO" */
constexpr unsigned BOOT_INFO_CMDLINE = 1024;

struct boot_info {
    uint64_t magic;                 /* BOOT_INFO_MAGIC */
    uint64_t dtb;                   /* physical; 0 if the firmware had none */
    uint64_t rsdp;                  /* physical; 0 if the firmware had none */

    /* The EFI memory map at ExitBootServices: physical, 'mmap_size'
     * bytes of descriptors 'mmap_desc_size' apart (kernel/efi.h). */
    uint64_t mmap, mmap_size, mmap_desc_size;
    uint32_t mmap_desc_version;
    uint32_t reserved;

    /* The GOP framebuffer, for V4; base 0 if none. */
    struct {
        uint64_t base, size;
        uint32_t width, height, stride, format;
    } fb;

    char cmdline[BOOT_INFO_CMDLINE];  /* the load options, NUL-terminated */
};

/* Look at what the firmware passed, physical 'firmware_data': remember a
 * boot-information block if it is one. Returns the device tree's
 * physical address, or 0. */
uint64_t bootinfo_init(uint64_t firmware_data);

/* The block, or nullptr if the kernel was given a bare device tree. */
const struct boot_info *bootinfo(void);

#endif /* KERNEL_BOOTINFO_H */
