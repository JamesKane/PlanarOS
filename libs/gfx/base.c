/*
 * libs/gfx/base.c -- gfx.library: its base and its interface
 *
 * V7 step 2; V8 step 3's paths and type. lib/gfx built as a shared
 * library image (lib.ld), with the kernel calls' stubs for its Areas,
 * and the process data's reader, by which it finds font.library. The
 * drawing calls are the scalar loops unless the CPU has a vector unit,
 * when the kernel points them at arch/<isa>/lib/gfx.c's (the
 * alternatives below), as it does exec's CopyMem.
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

    .BltMask = BltMask,
    .FillPath = FillPath,
    .FillGradient = FillGradient,
    .PathRoundRect = PathRoundRect,
    .InitTmpRas = InitTmpRas,
    .OpenTextFont = OpenTextFont,
    .CloseTextFont = CloseTextFont,
    .SetFont = SetFont,
    .TextLength = TextLength,
    .TextRun = TextRun,
};

static struct Interface *const interfaces[] = { (struct Interface *)&main_iface, nullptr };

static const struct Library base = {
    .lib_Name = "gfx.library",
    .lib_Version = 8,
    .lib_Revision = 3,
    .lib_IdString = "gfx.library 8.3 (26.10.2026)",
    .lib_Interfaces = interfaces,
};

static const struct LibraryAlt alts[] = {
    { (void *const *)&main_iface.RectFill, (void *)RectFillVector, LIBF_VECTOR },
    { (void *const *)&main_iface.BltBitMap, (void *)BltBitMapVector, LIBF_VECTOR },
    { (void *const *)&main_iface.DrawBevel, (void *)DrawBevelVector, LIBF_VECTOR },
    { (void *const *)&main_iface.Text, (void *)TextVector, LIBF_VECTOR },
    { (void *const *)&main_iface.PathMask, (void *)PathMaskVector, LIBF_VECTOR },
    { (void *const *)&main_iface.BltMask, (void *)BltMaskVector, LIBF_VECTOR },
    { (void *const *)&main_iface.FillPath, (void *)FillPathVector, LIBF_VECTOR },
    { (void *const *)&main_iface.FillGradient, (void *)FillGradientVector, LIBF_VECTOR },
    { (void *const *)&main_iface.TextRun, (void *)TextRunVector, LIBF_VECTOR },
};

const struct LibraryImage _library_image = {
    .li_Library = (struct Library *)&base,
    .li_Alts = alts,
    .li_NumAlts = sizeof alts / sizeof alts[0],
};
