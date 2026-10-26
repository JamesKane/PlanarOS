/*
 * libs/gfx/base.c -- gfx.library: its base and its interface
 *
 * V7 step 2. lib/gfx built as a shared library image (lib.ld), with the
 * kernel calls' stubs for AllocBitMap's Areas. RectFill, BltBitMap and
 * DrawBevel are the scalar loops unless the CPU has a vector unit, when
 * the kernel points them at arch/<isa>/lib/gfx.c's (the alternatives
 * below), as it does exec's CopyMem.
 */
#include <planar/libimage.h>

#include "../../lib/gfx/rows.h"

static const struct Library base;

static const struct GfxIFace main_iface = {
    .Data = { "main", 1, sizeof(struct GfxIFace), (struct Library *)&base },

    .AllocBitMap = AllocBitMap,
    .FreeBitMap = FreeBitMap,
    .GetBitMapAttr = GetBitMapAttr,
    .InitBitMap = InitBitMap,
    .InitRastPort = InitRastPort,

    .RectFill = RectFill,
    .BltBitMap = BltBitMap,
    .DrawBevel = DrawBevel,
    .Text = Text,

    .PathBounds = PathBounds,
    .PathMaskScratch = PathMaskScratch,
    .PathMask = PathMask,
};

static struct Interface *const interfaces[] = { (struct Interface *)&main_iface, nullptr };

static const struct Library base = {
    .lib_Name = "gfx.library",
    .lib_Version = 7,
    .lib_Revision = 3,
    .lib_IdString = "gfx.library 7.3 (26.10.2026)",
    .lib_Interfaces = interfaces,
};

static const struct LibraryAlt alts[] = {
    { (void *const *)&main_iface.RectFill, (void *)RectFillVector, LIBF_VECTOR },
    { (void *const *)&main_iface.BltBitMap, (void *)BltBitMapVector, LIBF_VECTOR },
    { (void *const *)&main_iface.DrawBevel, (void *)DrawBevelVector, LIBF_VECTOR },
};

const struct LibraryImage _library_image = {
    .li_Library = (struct Library *)&base,
    .li_Alts = alts,
    .li_NumAlts = sizeof alts / sizeof alts[0],
};
