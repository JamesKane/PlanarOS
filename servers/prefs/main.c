/*
 * servers/prefs/main.c -- Prefs: input's and intuition's settings, set
 * live, and saved (V8 step 7; docs §13.5, decision 5)
 *
 * hwdb starts it when the Workbench's Tools menu asks (HWDB_NEW_PREFS),
 * one at a time. One window (prefswin.h): each change goes to its server
 * at once, input's by INPUT_SET and intuition's by SetPrefs, each server
 * holding it to its range. Save writes both to SYS:Prefs/Env-Archive
 * (planar/prefs.h), where each server reads its own when it starts, and
 * ends Prefs; Use ends it keeping what's in force; Cancel, or the close
 * gadget, puts back what was in force when it opened, and ends it.
 *
 * display and power are said, not set (decision 5): the screen's mode,
 * until display can set one; and that power has no profiles, until it
 * has performance control.
 */
#include <stddef.h>

#include <planar/devices/display.h>
#include <planar/devices/input.h>
#include <planar/intuition.h>
#include <planar/prefs.h>
#include <planar/syscalls.h>
#include <planar/ui.h>
#include <planar/utility.h>

#include "log.h"
#include "prefswin.h"

void *memset(void *dst, int c, size_t n);

constexpr int64_t SIG_INPUT = 16, SIG_DOS = 17, SIG_DISPLAY = 18;   /* 22 is intuition.library's */
constexpr uint64_t MS = 1000000, SECOND = 1000 * MS;

static struct {
    struct GfxIFace *gfx;
    struct IntuitionIFace *intuition;
    struct UIIFace *iui;
    struct Intuition in;
    struct Window w;
    struct BitMap *bm;
    struct UI ui;
    struct InputPrefs ip, ip_was;
    struct IntuiPrefs np, np_was;
    char display[48], power[48];
} pf;

static void say(const char *s)
{
    KLog(s);
}

/* --- input, on a connection made for each call. --- */

static int64_t input_call(uint32_t op, struct InputPrefs *p)
{
    struct MsgPort mp;
    struct InputMsg *m;
    int64_t e;

    if ((e = ConnectPort(&mp, "input", SIG_INPUT, 2, 2 * sizeof *m)) != 0)
        return e;
    e = ERR_NOMEM;
    if ((m = AllocMsg(&mp, sizeof *m))) {
        memset(m, 0, sizeof *m);
        m->im_Msg.mn_Length = sizeof *m;
        m->im_Op = op;
        m->im_Prefs = *p;
        if ((e = DoMsgUntil(&mp, &m->im_Msg, CurrentTime() + SECOND)) == 0 && (e = m->im_Msg.mn_Error) == 0)
            *p = m->im_Prefs;
    }
    ClosePort(&mp);
    return e;
}

/* What display and power are said to be. */
static void said(void)
{
    struct MsgPort mp;
    struct DisplayMsg *m;

    SNPrintf(pf.display, sizeof pf.display, "%lu \xc3\x97 %lu, set by the firmware", (uint64_t)pf.in.in_Width,
             (uint64_t)pf.in.in_Height);
    if (ConnectPort(&mp, "display.0", SIG_DISPLAY, 2, 2 * sizeof *m) == 0) {
        if ((m = AllocMsg(&mp, sizeof *m))) {
            memset(m, 0, sizeof *m);
            m->dm_Msg.mn_Length = sizeof *m;
            m->dm_Op = DISPLAY_INFO;
            m->dm_Task = FindTask(nullptr);
            if (DoMsgUntil(&mp, &m->dm_Msg, CurrentTime() + SECOND) == 0 && m->dm_Msg.mn_Error == 0) {
                if (m->dm_RefreshMilliHz)
                    SNPrintf(pf.display, sizeof pf.display, "%lu \xc3\x97 %lu at %lu.%lu Hz", (uint64_t)m->dm_Width,
                             (uint64_t)m->dm_Height, (uint64_t)m->dm_RefreshMilliHz / 1000,
                             (uint64_t)m->dm_RefreshMilliHz % 1000 / 100);
                if (m->dm_Sync)
                    ReleaseArea((const void *)m->dm_Sync);  /* DISPLAY_INFO's, mapped for us */
            }
        }
        ClosePort(&mp);
    }
    if (ConnectPort(&mp, "power", SIG_DISPLAY, 2, 256) == 0) {
        ClosePort(&mp);
        SNPrintf(pf.power, sizeof pf.power, "No profiles yet: no DVFS");
    } else {
        SNPrintf(pf.power, sizeof pf.power, "No power server here");
    }
}

/* --- Saving. --- */

static int64_t save(void)
{
    struct TagItem it[] = {
        { PREFS_RepeatDelay, pf.ip.ip_RepeatDelay }, { PREFS_RepeatRate, pf.ip.ip_RepeatRate },
        { PREFS_PointerSpeed, pf.ip.ip_PointerSpeed }, { PREFS_PointerAccel, pf.ip.ip_PointerAccel },
        { TAG_END, 0 },
    };
    struct TagItem nt[] = {
        { PREFS_Hud, pf.np.ip_Hud }, { PREFS_PointerSize, pf.np.ip_PointerSize },
        { PREFS_BackdropTop, pf.np.ip_BackdropTop }, { PREFS_BackdropBottom, pf.np.ip_BackdropBottom },
        { TAG_END, 0 },
    };
    struct Dos dos;
    int64_t e;

    if ((e = OpenDos(&dos, SIG_DOS)) != 0)
        return e;
    if ((e = WritePrefs(&dos, "input", it)) == 0)
        e = WritePrefs(&dos, "intuition", nt);
    CloseDos(&dos);
    return e;
}

/* --- Changes. --- */

static void set_input(void)
{
    struct InputPrefs p = pf.ip;
    int64_t e = input_call(INPUT_SET, &p);

    if (e != 0)
        KPrintF("prefs: input refused its settings (%ld)", e);
}

static void set_intuition(void)
{
    int64_t e = pf.intuition->SetPrefs(&pf.in, &pf.np);

    if (e != 0)
        KPrintF("prefs: intuition refused its settings (%ld)", e);
}

/* What was in force when Prefs opened, put back. */
static void cancel(void)
{
    pf.ip = pf.ip_was;
    pf.np = pf.np_was;
    set_input();
    set_intuition();
}

/* A gadget's change: false once Prefs is to end. */
static bool change(const struct UIChange *c)
{
    uint32_t v = (uint32_t)c->uc_Value;
    int64_t e;

    switch (c->uc_Gadget) {
    case PW_DELAY:
        pf.ip.ip_RepeatDelay = v;
        set_input();
        break;
    case PW_RATE:
        pf.ip.ip_RepeatRate = v;
        set_input();
        break;
    case PW_SPEED:
        pf.ip.ip_PointerSpeed = v;
        set_input();
        break;
    case PW_ACCEL:
        pf.ip.ip_PointerAccel = v;
        set_input();
        break;
    case PW_HUD:
        pf.np.ip_Hud = v;
        set_intuition();
        break;
    case PW_SIZE:
        pf.np.ip_PointerSize = v;
        set_intuition();
        break;
    case PW_THEME:
        pf.np.ip_BackdropTop = pw_theme_top[v % 3];
        pf.np.ip_BackdropBottom = pw_theme_bottom[v % 3];
        set_intuition();
        break;
    case PW_CANCEL:
        cancel();
        return false;
    case PW_USE:
        return false;
    case PW_SAVE:
        if ((e = save()) != 0) {
            struct EasyStruct es = { sizeof es, 0, "Prefs", "The settings couldn't be saved\nto SYS:Prefs/Env-Archive.",
                                     "OK" };

            KPrintF("prefs: the settings couldn't be saved (%ld)", e);
            pf.intuition->EasyRequest(&pf.in, &pf.w, &es);
            return true;
        }
        say("prefs: the settings saved to SYS:Prefs/Env-Archive");
        return false;
    default:
        break;
    }
    return true;
}

/* --- Starting. --- */

static bool open_libs(void)
{
    struct Library *lib;

    return (lib = IExec->OpenLibrary("gfx.library", 8)) &&
           (pf.gfx = (struct GfxIFace *)IExec->GetInterface(lib, "main", 1)) &&
           (lib = IExec->OpenLibrary("intuition.library", 8)) &&
           (pf.intuition = (struct IntuitionIFace *)IExec->GetInterface(lib, "main", 1)) &&
           pf.intuition->Data.if_Size >= offsetof(struct IntuitionIFace, SetPrefs) + sizeof(void *) &&
           (lib = IExec->OpenLibrary("ui.library", 8)) &&
           (pf.iui = (struct UIIFace *)IExec->GetInterface(lib, "main", 1));
}

int main(uint64_t)
{
    struct IntuiEvent ev;
    struct UIChange c;
    bool going = true;
    int64_t e;

    if (!open_libs()) {
        say("prefs: no gfx.library, intuition.library 8.7 or ui.library");
        return 1;
    }
    if ((e = pf.intuition->OpenIntuition(&pf.in, CurrentTime() + 5 * SECOND)) != 0) {
        KPrintF("prefs: no intuition (%ld)", e);
        return 1;
    }
    if ((e = input_call(INPUT_GET, &pf.ip)) != 0 || (e = pf.intuition->GetPrefs(&pf.in, &pf.np)) != 0) {
        KPrintF("prefs: input's or intuition's settings couldn't be read (%ld)", e);
        return 1;
    }
    pf.ip_was = pf.ip;
    pf.np_was = pf.np;
    said();
    if (!(pf.bm = pf.gfx->AllocBitMap(PW_WIDTH, PW_HEIGHT, PIXFMT_XRGB8888, 0, nullptr)) ||
        pf.iui->InitUI(&pf.ui, pf.gfx) != 0 || !pw_build(pf.iui, &pf.ui, &pf.ip, &pf.np, pf.display, pf.power)) {
        say("prefs: no memory for its window");
        return 1;
    }
    e = OpenWindowTags(pf.intuition, &pf.in, &pf.w, { WA_Left, PW_LEFT }, { WA_Top, PW_TOP },
                       { WA_Title, (uint64_t)"Prefs" },
                       { WA_Flags, WFLG_DRAGBAR | WFLG_DEPTHGADGET | WFLG_CLOSEGADGET | WFLG_ACTIVATE },
                       { WA_IDCMP, IDCMP_MOUSEBUTTONS | IDCMP_MOUSEMOVE | IDCMP_VANILLAKEY | IDCMP_RAWKEY |
                                   IDCMP_CLOSEWINDOW },
                       { WA_BitMap, (uint64_t)pf.bm });
    if (e != 0) {
        KPrintF("prefs: no window (%ld)", e);
        return 1;
    }
    pf.ui.ui_RPort = &pf.w.w_RPort;
    pf.iui->LayoutUI(&pf.ui, PW_WIDTH, PW_HEIGHT);
    pf.iui->DrawUI(&pf.ui);
    pf.ui.ui_NumDamage = 0;
    if (pf.intuition->PresentWindow(&pf.w, nullptr, 0) != 0)
        return 3;
    while (going) {
        if ((e = pf.intuition->WaitIntuiEvent(&pf.in, CurrentTime() + SECOND)) == ERR_PORT_DIED ||
            PortGone(&pf.in.in_Port)) {
            say("prefs: intuition went: ending");
            return 3;
        }
        while (going && pf.intuition->GetIntuiEvent(&pf.in, &ev)) {
            if (ev.iev_Window != pf.w.w_Id)
                continue;
            if (ev.iev_Class == IDCMP_CLOSEWINDOW) {
                cancel();
                going = false;
                break;
            }
            if (pf.intuition->WaitSafe(&pf.w, CurrentTime() + SECOND) == ERR_PORT_DIED)
                return 3;
            if (pf.iui->HandleUIEvent(&pf.ui, &ev, &c))
                going = change(&c);
            if (pf.ui.ui_NumDamage && pf.intuition->PresentWindow(&pf.w, pf.ui.ui_Damage, pf.ui.ui_NumDamage) != 0)
                return 3;
            pf.ui.ui_NumDamage = 0;
        }
    }
    pf.intuition->CloseWindow(&pf.w);
    return 0;
}
