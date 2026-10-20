/*
 * kernel/efi.h -- the parts of UEFI the kernel and its EFI stub use
 *
 * From the UEFI specification (2.10): the memory map's descriptors,
 * which the kernel reads (memmap.c), and the tables and protocols the
 * stub calls before ExitBootServices (arch/<arch>/efi.c). Only the
 * members the stub uses have types; the rest are placeholders, so the
 * offsets stay right.
 */
#ifndef KERNEL_EFI_H
#define KERNEL_EFI_H

#include <stddef.h>
#include <stdint.h>

typedef uint64_t efi_status;
typedef void *efi_handle;
typedef uint16_t efi_char16;

constexpr efi_status EFI_SUCCESS          = 0;
constexpr efi_status EFI_ERROR            = 1ULL << 63;
constexpr efi_status EFI_LOAD_ERROR       = EFI_ERROR | 1;
constexpr efi_status EFI_INVALID_PARAMETER = EFI_ERROR | 2;
constexpr efi_status EFI_BUFFER_TOO_SMALL = EFI_ERROR | 5;

struct efi_guid {
    uint32_t a;
    uint16_t b, c;
    uint8_t d[8];
};

/* --- The memory map. --- */

enum : uint32_t {
    EFI_RESERVED_MEMORY, EFI_LOADER_CODE, EFI_LOADER_DATA, EFI_BOOT_SERVICES_CODE,
    EFI_BOOT_SERVICES_DATA, EFI_RUNTIME_SERVICES_CODE, EFI_RUNTIME_SERVICES_DATA,
    EFI_CONVENTIONAL_MEMORY, EFI_UNUSABLE_MEMORY, EFI_ACPI_RECLAIM_MEMORY,
    EFI_ACPI_MEMORY_NVS, EFI_MEMORY_MAPPED_IO, EFI_MEMORY_MAPPED_IO_PORT_SPACE,
    EFI_PAL_CODE, EFI_PERSISTENT_MEMORY, EFI_UNACCEPTED_MEMORY,
};

constexpr uint64_t EFI_MEMORY_WB = 0x8;     /* the range can be write-back */
constexpr uint64_t EFI_PAGE_SIZE = 4096;

/* One descriptor. The firmware's are 'desc_size' apart, which may be
 * more than sizeof this. */
struct efi_memory_descriptor {
    uint32_t type;
    uint32_t pad;
    uint64_t physical_start;
    uint64_t virtual_start;
    uint64_t pages;
    uint64_t attribute;
};

/* --- Tables and protocols (the stub's). --- */

struct efi_table_header {
    uint64_t signature;
    uint32_t revision, header_size, crc32, reserved;
};

struct efi_simple_text_output {
    void *reset;
    efi_status (*output_string)(struct efi_simple_text_output *self, const efi_char16 *s);
};

enum : uint32_t { EFI_ALLOCATE_ANY_PAGES, EFI_ALLOCATE_MAX_ADDRESS, EFI_ALLOCATE_ADDRESS };

struct efi_boot_services {
    struct efi_table_header hdr;
    void *raise_tpl, *restore_tpl;
    efi_status (*allocate_pages)(uint32_t type, uint32_t memory_type, uint64_t pages,
                                 uint64_t *address);
    efi_status (*free_pages)(uint64_t address, uint64_t pages);
    efi_status (*get_memory_map)(uint64_t *size, struct efi_memory_descriptor *map,
                                 uint64_t *key, uint64_t *desc_size, uint32_t *desc_version);
    void *allocate_pool, *free_pool;
    void *create_event, *set_timer, *wait_for_event, *signal_event, *close_event,
         *check_event;
    void *install_protocol_interface, *reinstall_protocol_interface,
         *uninstall_protocol_interface;
    efi_status (*handle_protocol)(efi_handle h, const struct efi_guid *protocol,
                                  void **interface);
    void *reserved, *register_protocol_notify, *locate_handle, *locate_device_path,
         *install_configuration_table;
    void *load_image, *start_image, *exit, *unload_image;
    efi_status (*exit_boot_services)(efi_handle image, uint64_t key);
    void *get_next_monotonic_count, *stall, *set_watchdog_timer;
    void *connect_controller, *disconnect_controller;
    void *open_protocol, *close_protocol, *open_protocol_information;
    void *protocols_per_handle, *locate_handle_buffer;
    efi_status (*locate_protocol)(const struct efi_guid *protocol, void *registration,
                                  void **interface);
};

struct efi_configuration_table {
    struct efi_guid guid;
    void *table;
};

struct efi_system_table {
    struct efi_table_header hdr;
    efi_char16 *firmware_vendor;
    uint32_t firmware_revision;
    efi_handle console_in_handle;
    void *con_in;
    efi_handle console_out_handle;
    struct efi_simple_text_output *con_out;
    efi_handle standard_error_handle;
    void *std_err;
    void *runtime_services;
    struct efi_boot_services *boot_services;
    uint64_t tables;
    struct efi_configuration_table *table;
};

struct efi_loaded_image {
    uint32_t revision;
    efi_handle parent;
    struct efi_system_table *system_table;
    efi_handle device;
    void *file_path;
    void *reserved;
    uint32_t load_options_size;     /* bytes */
    void *load_options;
    void *image_base;
    uint64_t image_size;
    uint32_t image_code_type, image_data_type;
    void *unload;
};

struct efi_gop_mode_info {
    uint32_t version, width, height, pixel_format;
    uint32_t pixel_masks[4];
    uint32_t pixels_per_line;
};

struct efi_gop_mode {
    uint32_t max_mode, mode;
    struct efi_gop_mode_info *info;
    uint64_t info_size;
    uint64_t frame_buffer_base, frame_buffer_size;
};

struct efi_gop {
    void *query_mode, *set_mode, *blt;
    struct efi_gop_mode *mode;
};

#define EFI_LOADED_IMAGE_GUID \
    { 0x5b1b31a1, 0x9562, 0x11d2, { 0x8e, 0x3f, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }
#define EFI_GOP_GUID \
    { 0x9042a9de, 0x23dc, 0x4a38, { 0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a } }
#define EFI_DTB_TABLE_GUID \
    { 0xb1b621d5, 0xf19c, 0x41a5, { 0x83, 0x0b, 0xd9, 0x15, 0x2c, 0x69, 0xaa, 0xe0 } }
#define EFI_ACPI_20_TABLE_GUID \
    { 0x8868e871, 0xe4f1, 0x11d3, { 0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81 } }

#endif /* KERNEL_EFI_H */
