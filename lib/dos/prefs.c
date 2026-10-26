/*
 * lib/dos/prefs.c -- ReadPrefs and WritePrefs: a server's saved settings,
 * SYS:Prefs/Env-Archive/<server>.prefs (planar/prefs.h, V8 step 7)
 */
#include <planar/prefs.h>
#include <planar/syscalls.h>

static bool path_of(char *path, unsigned size, const char *server)
{
    return SNPrintf(path, size, "SYS:Prefs/Env-Archive/%s.prefs", server) < size;
}

int64_t ReadPrefs(struct Dos *dos, const char *server, struct TagItem *tags, uint32_t max)
{
    struct PrefsHeader h;
    struct TagItem t[PREFS_TAGS];
    char path[DOS_PATH];
    uint32_t n = 0;
    int64_t f, e;

    if (!max)
        return ERR_BADARG;
    tags[0] = (struct TagItem){ TAG_END, 0 };
    if (!path_of(path, sizeof path, server))
        return ERR_BADARG;
    if ((f = OpenFile(dos, path, 0, nullptr)) < 0)
        return f;
    e = ReadFile(dos, (uint32_t)f, 0, &h, sizeof h);
    if (e == (int64_t)sizeof h && h.ph_Magic == PREFS_MAGIC && h.ph_Version >= 1 && h.ph_Count <= PREFS_TAGS)
        e = ReadFile(dos, (uint32_t)f, sizeof h, t, h.ph_Count * (uint32_t)sizeof t[0]);
    else if (e >= 0)
        e = ERR_BADARG;
    CloseFile(dos, (uint32_t)f);
    if (e < 0)
        return e;
    if (e != (int64_t)(h.ph_Count * sizeof t[0]))
        return ERR_BADARG;
    for (uint32_t i = 0; i < h.ph_Count && n < max - 1; i++)
        if (t[i].ti_Tag != TAG_END)
            tags[n++] = t[i];
    tags[n] = (struct TagItem){ TAG_END, 0 };
    return n;
}

int64_t WritePrefs(struct Dos *dos, const char *server, const struct TagItem *tags)
{
    struct {
        struct PrefsHeader h;
        struct TagItem t[PREFS_TAGS];
    } file = { { PREFS_MAGIC, PREFS_VERSION, 0, 0 }, {} };
    char path[DOS_PATH];
    uint32_t size;
    int64_t f, e;

    while (tags[file.h.ph_Count].ti_Tag != TAG_END)
        if (file.h.ph_Count == PREFS_TAGS)
            return ERR_BADARG;
        else
            file.t[file.h.ph_Count] = tags[file.h.ph_Count], file.h.ph_Count++;
    if (!path_of(path, sizeof path, server))
        return ERR_BADARG;
    if ((e = CreateDir(dos, "SYS:Prefs")) != 0 && e != ERR_EXISTS)
        return e;
    if ((e = CreateDir(dos, "SYS:Prefs/Env-Archive")) != 0 && e != ERR_EXISTS)
        return e;
    if ((f = OpenFile(dos, path, DOSF_WRITE | DOSF_CREATE, nullptr)) < 0)
        return f;
    size = (uint32_t)(sizeof file.h + file.h.ph_Count * sizeof file.t[0]);
    e = WriteFile(dos, (uint32_t)f, 0, &file, size);
    CloseFile(dos, (uint32_t)f);
    return e < 0 ? e : e == (int64_t)size ? 0 : ERR_FULL;
}
