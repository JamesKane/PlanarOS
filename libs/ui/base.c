/*
 * libs/ui/base.c -- ui.library: its base and its interface
 *
 * V8 step 6. lib/ui built as a shared library image (lib.ld), with
 * utility's tag lists linked in. It keeps no state: a UI is the
 * caller's, and draws through the caller's gfx.library.
 */
#include <planar/libimage.h>
#include <planar/ui.h>

static const struct Library base;

static const struct UIIFace main_iface = {
    .Data = { "main", 1, sizeof(struct UIIFace), (struct Library *)&base },

    .InitUI = InitUI,
    .FreeUI = FreeUI,
    .AddGadget = AddGadget,
    .SetGadgetAttrsA = SetGadgetAttrsA,
    .LayoutUI = LayoutUI,
    .DrawUI = DrawUI,
    .HandleUIEvent = HandleUIEvent,
};

static struct Interface *const interfaces[] = { (struct Interface *)&main_iface, nullptr };

static const struct Library base = {
    .lib_Name = "ui.library",
    .lib_Version = 8,
    .lib_Revision = 6,
    .lib_IdString = "ui.library 8.6 (26.10.2026)",
    .lib_Interfaces = interfaces,
};

const struct LibraryImage _library_image = {
    .li_Library = (struct Library *)&base,
};
