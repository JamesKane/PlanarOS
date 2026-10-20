/*
 * vectra/endian.h -- explicit-endian loads
 *
 * Firmware formats fix their byte order (FDT is big-endian, ACPI little-
 * endian) whatever the CPU's. Byte loads keep these portable and safe on
 * unaligned or Device memory; the compiler folds them into one load (and a
 * byte swap) where it can.
 */
#ifndef VECTRA_ENDIAN_H
#define VECTRA_ENDIAN_H

#include <stdint.h>

static inline uint32_t get_be32(const void *p)
{
    const uint8_t *b = p;

    return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
}

static inline uint64_t get_be64(const void *p)
{
    const uint8_t *b = p;

    return (uint64_t)get_be32(b) << 32 | get_be32(b + 4);
}

static inline uint32_t get_le32(const void *p)
{
    const uint8_t *b = p;

    return (uint32_t)b[3] << 24 | (uint32_t)b[2] << 16 | (uint32_t)b[1] << 8 | b[0];
}

static inline uint64_t get_le64(const void *p)
{
    const uint8_t *b = p;

    return (uint64_t)get_le32(b + 4) << 32 | get_le32(b);
}

#endif /* VECTRA_ENDIAN_H */
