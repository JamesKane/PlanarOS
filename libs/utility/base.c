/*
 * libs/utility/base.c -- utility.library: its base and its interface
 *
 * V6 step 4. lib/utility built as a shared library image (lib.ld), with
 * its own copy of lib/exec/fmt.c for SNPrintf: a library can't call
 * another's functions but through an interface, and one it found itself
 * would be state it can't keep.
 */
#include <planar/libimage.h>
#include <planar/utility.h>

static const struct Library base;

static const struct UtilityIFace main_iface = {
    .Data = { "main", 1, sizeof(struct UtilityIFace), (struct Library *)&base },

    .NextTagItem = NextTagItem,
    .FindTagItem = FindTagItem,
    .GetTagData = GetTagData,
    .TagInArray = TagInArray,
    .CallHookPkt = CallHookPkt,

    .ToUpper = ToUpper,
    .ToLower = ToLower,
    .Stricmp = Stricmp,
    .Strnicmp = Strnicmp,
    .VSNPrintf = VSNPrintf,
    .SNPrintf = SNPrintf,

    .StrFromC = StrFromC,
    .StrSlice = StrSlice,
    .StrTrim = StrTrim,
    .StrSplit = StrSplit,
    .StrSame = StrSame,
    .StrSameNoCase = StrSameNoCase,
    .StrHasPrefix = StrHasPrefix,
    .StrToU64 = StrToU64,

    .SpanSlice = SpanSlice,
    .SpanReadU8 = SpanReadU8,
    .SpanReadU16LE = SpanReadU16LE,
    .SpanReadU32LE = SpanReadU32LE,
    .SpanReadU64LE = SpanReadU64LE,
    .SpanReadU16BE = SpanReadU16BE,
    .SpanReadU32BE = SpanReadU32BE,
    .SpanReadU64BE = SpanReadU64BE,
};

static struct Interface *const interfaces[] = { (struct Interface *)&main_iface, nullptr };

static const struct Library base = {
    .lib_Name = "utility.library",
    .lib_Version = 6,
    .lib_Revision = 4,
    .lib_IdString = "utility.library 6.4 (22.10.2026)",
    .lib_Interfaces = interfaces,
};

const struct LibraryImage _library_image = {
    .li_Library = (struct Library *)&base,
};
