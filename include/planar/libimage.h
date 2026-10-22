/*
 * planar/libimage.h -- a shared library's image, as the kernel loads it
 *
 * V6 step 4. A library is linked at 0 as a position-independent image
 * of code and constants only (arch/<isa>/user/lib.ld refuses data). It
 * starts with a header of offsets from its start (lib/libhead.S): the
 * magic, its size, its relocations, and its LibraryImage. At boot the
 * kernel copies each into the shared library region, applies the
 * relocations, and then reads the LibraryImage, whose pointers are by
 * then the region's addresses. Private to the kernel and the libraries.
 */
#ifndef PLANAR_LIBIMAGE_H
#define PLANAR_LIBIMAGE_H

#include <stdint.h>

#include <planar/exec.h>

constexpr uint32_t LIBIMAGE_MAGIC = 0x42494c50;     /* "PLIB" */
constexpr uint32_t LIBIMAGE_FORMAT = 1;

/* The header, at offset 0. */
struct LibraryHeader {
    uint32_t lh_Magic;
    uint32_t lh_Format;
    uint64_t lh_Size;               /* bytes: all code and constants */
    uint64_t lh_RelaStart, lh_RelaEnd;
    uint64_t lh_Image;              /* the LibraryImage */
};

/* What the CPU must have for an alternative (arch_has_vector). */
constexpr uint64_t LIBF_VECTOR = 1u << 0;

/* A table entry to point at another function if the CPU has what it
 * needs: exec's CopyMem by the vector unit, say. */
struct LibraryAlt {
    void *const *la_Slot;
    void *la_Function;
    uint64_t la_Needs;              /* LIBF_* */
};

struct LibraryImage {
    struct Library *li_Library;
    const struct LibraryAlt *li_Alts;
    uint64_t li_NumAlts;
    /* exec.library's alone: every library's base, filled in at boot, for
     * OpenLibrary. Slots past the last library stay nullptr. */
    struct Library *const volatile *li_Directory;
    uint64_t li_DirectorySlots;
};

#endif /* PLANAR_LIBIMAGE_H */
