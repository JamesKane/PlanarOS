/*
 * libs/intuition/base.c -- intuition.library: its base and its interface
 *
 * V7 step 3, decision 5. lib/intuition built as a shared library image
 * (lib.ld), with what it calls linked in: lib/exec's messages, utility's
 * tag lists, and the kernel calls' stubs. Its state is the caller's
 * Intuition and Windows.
 */
#include <planar/intuition.h>
#include <planar/libimage.h>

static const struct Library base;

static const struct IntuitionIFace main_iface = {
    .Data = { "main", 1, sizeof(struct IntuitionIFace), (struct Library *)&base },

    .OpenIntuition = OpenIntuition,
    .CloseIntuition = CloseIntuition,
    .OpenWindowTagList = OpenWindowTagList,
    .CloseWindow = CloseWindow,
    .PresentWindow = PresentWindow,
    .WaitSafe = WaitSafe,
};

static struct Interface *const interfaces[] = { (struct Interface *)&main_iface, nullptr };

static const struct Library base = {
    .lib_Name = "intuition.library",
    .lib_Version = 7,
    .lib_Revision = 3,
    .lib_IdString = "intuition.library 7.3 (22.10.2026)",
    .lib_Interfaces = interfaces,
};

const struct LibraryImage _library_image = {
    .li_Library = (struct Library *)&base,
};
