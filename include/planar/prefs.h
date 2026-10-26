/*
 * planar/prefs.h -- settings saved by Prefs, read back by their servers
 * (V8 step 7)
 *
 * Each server that has settings a user sets keeps them in a file of its
 * own, SYS:Prefs/Env-Archive/<server>.prefs: input's ("input") and
 * intuition's ("intuition"). Prefs writes them on Save; each server
 * reads its own when it starts, so a setting outlives the server's
 * death. A file is small and tagged: a PrefsHeader, then ph_Count
 * TagItems. A reader takes the tags it knows and passes over the rest,
 * so a later version's file is read as far as it can be; a file of
 * another magic, or a version before 1, isn't read at all.
 *
 * dos.library's (lib/dos/prefs.c): ReadPrefs fills 'tags' with up to
 * max - 1 of the file's, ended by TAG_END: how many, or why not
 * (ERR_NOTFOUND: none saved; ERR_BADARG: not a prefs file). WritePrefs
 * writes 'tags', ended by TAG_END, making SYS:Prefs and its
 * Env-Archive if they aren't there.
 */
#ifndef PLANAR_PREFS_H
#define PLANAR_PREFS_H

#include <stdint.h>

#include <planar/dos.h>
#include <planar/utility.h>

constexpr uint32_t PREFS_MAGIC = 0x53465250;    /* "PRFS" */
constexpr uint32_t PREFS_VERSION = 1;
constexpr unsigned PREFS_TAGS = 16;             /* a file's, at most */

struct PrefsHeader {
    uint32_t ph_Magic;
    uint32_t ph_Version;
    uint32_t ph_Count;                  /* TagItems after it */
    uint32_t ph_Pad;
};

/* The tags: each server's own range. */
constexpr Tag PREFS_Dummy = TAG_USER + 0x700;
/* input's: planar/devices/input.h's InputPrefs, field by field */
constexpr Tag PREFS_RepeatDelay = PREFS_Dummy + 0x01;
constexpr Tag PREFS_RepeatRate = PREFS_Dummy + 0x02;
constexpr Tag PREFS_PointerSpeed = PREFS_Dummy + 0x03;
constexpr Tag PREFS_PointerAccel = PREFS_Dummy + 0x04;
/* intuition's: planar/intuition.h's IntuiPrefs */
constexpr Tag PREFS_Hud = PREFS_Dummy + 0x11;
constexpr Tag PREFS_PointerSize = PREFS_Dummy + 0x12;
constexpr Tag PREFS_BackdropTop = PREFS_Dummy + 0x13;
constexpr Tag PREFS_BackdropBottom = PREFS_Dummy + 0x14;

int64_t ReadPrefs(struct Dos *dos, const char *server, struct TagItem *tags, uint32_t max);
int64_t WritePrefs(struct Dos *dos, const char *server, const struct TagItem *tags);

#endif /* PLANAR_PREFS_H */
