/*
 * conformance/typescene.h -- a little of gfx.library's V8 step 3 in one
 * picture
 *
 * A gradient backdrop, a bar filled with another along a rounded
 * rectangle, a ring (two rounded rectangles, even-odd), and a line each
 * of System Sans bold at 16 px, System Sans and System Mono at 13, and
 * box drawing. typecheck.h holds its CRC to the host's; the window test
 * (conformance/window.c) shows it in a window and reads it back from
 * the screen.
 */
#ifndef CONFORMANCE_TYPESCENE_H
#define CONFORMANCE_TYPESCENE_H

#include <planar/gfx.h>

constexpr uint32_t TC_SCENE_W = 160, TC_SCENE_H = 100;

[[maybe_unused]] static struct TextFont *tc_open(const struct GfxIFace *g, const char *name, uint16_t size,
                                                 uint8_t style)
{
    struct TextAttr ta = { name, size, style, 0 };

    return g->OpenTextFont(&ta);
}

/* A little of everything, into a bitmap TC_SCENE_W by TC_SCENE_H: false
 * if a font won't open. */
[[maybe_unused]] static bool tc_scene(const struct GfxIFace *g, struct RastPort *rp)
{
    static struct GfxPoint pts[48];
    static uint8_t verbs[48];
    struct GfxPath path = { pts, verbs, 48, 48, 0, 0 };
    struct GfxGradient back = { { 0, 0, TC_SCENE_W - 1, TC_SCENE_H - 1 }, 0xff2a3a55u, 0xff121822u, 0 };
    struct GfxGradient bar = { { 8, 0, TC_SCENE_W - 9, 0 }, 0xffd8dee8u, 0xff8bb4e8u, GRADF_HORIZONTAL };
    struct TextFont *sans = tc_open(g, nullptr, 13, 0), *mono = tc_open(g, "System Mono", 13, 0);
    struct TextFont *bold = tc_open(g, "System Sans", 16, FSF_BOLD);

    if (!sans || !mono || !bold) {
        g->CloseTextFont(sans);
        g->CloseTextFont(mono);
        g->CloseTextFont(bold);
        return false;
    }
    g->FillGradient(rp, &back, nullptr, 0);
    g->PathRoundRect(&path, &(struct GfxRect){ 8, 6, TC_SCENE_W - 9, 30 }, 6 * 64);
    g->FillGradient(rp, &bar, &path, 0);
    path.gp_NumPoints = path.gp_NumVerbs = 0;
    g->PathRoundRect(&path, &(struct GfxRect){ 110, 52, 150, 92 }, 20 * 64);
    g->PathRoundRect(&path, &(struct GfxRect){ 120, 62, 140, 82 }, 10 * 64);
    rp->rp_FgPen = 0xffe0a040u;
    g->FillPath(rp, &path, PATHF_EVENODD);
    rp->rp_DrawMode = JAM1;
    rp->rp_FgPen = 0xff121822u;
    g->SetFont(rp, bold);
    g->Text(rp, 14, 9, "Planar 8", 8);
    rp->rp_FgPen = 0xffe8eef6u;
    g->SetFont(rp, sans);
    g->Text(rp, 8, 36, "Type, at last: AVA ffi", 22);
    g->SetFont(rp, mono);
    g->Text(rp, 8, 54, "1> List SYS:", 12);
    rp->rp_FgPen = 0xff8bb4e8u;
    g->Text(rp, 8, 70, "\xe2\x94\x8c\xe2\x94\x80\xe2\x94\x90 \xe2\x96\x88", 13);
    g->SetFont(rp, nullptr);
    g->CloseTextFont(sans);
    g->CloseTextFont(mono);
    g->CloseTextFont(bold);
    return true;
}

#endif /* CONFORMANCE_TYPESCENE_H */
