/*
 * lib/dos/dos.c -- dos.library: the dos server's client (planar/dos.h)
 */
#include <stddef.h>

#include <planar/dos.h>
#include <planar/syscalls.h>

int64_t OpenDos(struct Dos *d, int64_t sigbit)
{
    return OpenDosUntil(d, sigbit, 0);
}

int64_t OpenDosUntil(struct Dos *d, int64_t sigbit, uint64_t deadline)
{
    int64_t e;

    memset(d, 0, sizeof *d);
    if ((e = ConnectPortUntil(&d->ds_Port, "dos", sigbit, 4, sizeof *d->ds_Msg + DOS_MAX_IO + 64,
                              deadline)) != 0)
        return e;
    d->ds_Open = true;
    if (!(d->ds_Msg = AllocMsg(&d->ds_Port, sizeof *d->ds_Msg)) ||
        !(d->ds_Buffer = AllocMsg(&d->ds_Port, DOS_MAX_IO))) {
        CloseDos(d);
        return ERR_NOMEM;
    }
    return 0;
}

void CloseDos(struct Dos *d)
{
    if (d->ds_Open)
        ClosePort(&d->ds_Port);
    d->ds_Open = false;
}

static struct DosMsg *msg(struct Dos *d, uint32_t op)
{
    memset(d->ds_Msg, 0, offsetof(struct DosMsg, dm_Path));
    d->ds_Msg->dm_Msg.mn_Length = sizeof *d->ds_Msg;
    d->ds_Msg->dm_Version = DOS_VERSION;
    d->ds_Msg->dm_Op = op;
    d->ds_Msg->dm_Path[0] = '\0';
    return d->ds_Msg;
}

static void set_path(struct DosMsg *m, const char *path)
{
    unsigned i = 0;

    for (; path[i] && i < DOS_PATH - 1; i++)
        m->dm_Path[i] = path[i];
    m->dm_Path[i] = '\0';
}

static int64_t call(struct Dos *d)
{
    int64_t e;

    if (!d->ds_Open)
        return ERR_PORT_DIED;
    e = DoMsgUntil(&d->ds_Port, &d->ds_Msg->dm_Msg, CurrentTime() + DOS_DEADLINE_MS * 1000000);
    if (e == ERR_TIMEOUT)
        ReportUnresponsive("dos");  /* and the message is dos's until it answers */
    return e ? e : d->ds_Msg->dm_Msg.mn_Error;
}

static void take_entry(const struct DosMsg *m, struct DosEntry *e)
{
    if (e) {
        memcpy(e, &m->dm_Entry, sizeof *e);
        e->de_Name[DOS_PATH - 1] = '\0';
    }
}

int64_t OpenFile(struct Dos *d, const char *path, uint32_t flags, struct DosEntry *entry)
{
    struct DosMsg *m = msg(d, DOS_OPEN);
    int64_t e;

    set_path(m, path);
    m->dm_Flags = flags;
    if ((e = call(d)))
        return e;
    take_entry(m, entry);
    return m->dm_File;
}

int64_t CloseFile(struct Dos *d, uint32_t file)
{
    msg(d, DOS_CLOSE)->dm_File = file;
    return call(d);
}

int64_t ReadFile(struct Dos *d, uint32_t file, uint64_t off, void *buf, uint32_t len)
{
    uint32_t done = 0;

    while (done < len) {
        struct DosMsg *m = msg(d, DOS_READ);
        uint32_t n = len - done < DOS_MAX_IO ? len - done : DOS_MAX_IO;
        int64_t e;

        m->dm_File = file;
        m->dm_Offset = off + done;
        m->dm_Length = n;
        m->dm_Buffer = (uint64_t)d->ds_Buffer;
        if ((e = call(d)))
            return e;
        if (m->dm_Length > n)
            return ERR_BADARG;      /* the server's answer, checked */
        memcpy((uint8_t *)buf + done, d->ds_Buffer, m->dm_Length);
        done += m->dm_Length;
        if (m->dm_Length < n)
            break;                  /* the end of the file */
    }
    return done;
}

int64_t WriteFile(struct Dos *d, uint32_t file, uint64_t off, const void *buf, uint32_t len)
{
    uint32_t done = 0;

    while (done < len) {
        struct DosMsg *m = msg(d, DOS_WRITE);
        uint32_t n = len - done < DOS_MAX_IO ? len - done : DOS_MAX_IO;
        int64_t e;

        memcpy(d->ds_Buffer, (const uint8_t *)buf + done, n);
        m->dm_File = file;
        m->dm_Offset = off + done;
        m->dm_Length = n;
        m->dm_Buffer = (uint64_t)d->ds_Buffer;
        if ((e = call(d)))
            return e;
        if (m->dm_Length != n)
            return ERR_FULL;
        done += n;
    }
    return done;
}

int64_t ExaminePath(struct Dos *d, const char *path, struct DosEntry *entry)
{
    struct DosMsg *m = msg(d, DOS_STAT);
    int64_t e;

    set_path(m, path);
    if ((e = call(d)))
        return e;
    take_entry(m, entry);
    return 0;
}

int64_t ExamineNext(struct Dos *d, uint32_t dir, uint32_t *cursor, struct DosEntry *entry)
{
    struct DosMsg *m = msg(d, DOS_NEXT);
    int64_t e;

    m->dm_File = dir;
    m->dm_Cursor = *cursor;
    if ((e = call(d)))
        return e;
    *cursor = m->dm_Cursor;
    take_entry(m, entry);
    return 0;
}

int64_t DeletePath(struct Dos *d, const char *path)
{
    set_path(msg(d, DOS_REMOVE), path);
    return call(d);
}

int64_t CreateDir(struct Dos *d, const char *path)
{
    set_path(msg(d, DOS_MKDIR), path);
    return call(d);
}

int64_t NextVolume(struct Dos *d, uint32_t index, struct DosEntry *entry)
{
    struct DosMsg *m = msg(d, DOS_VOLUME);
    int64_t e;

    m->dm_Cursor = index;
    if ((e = call(d)))
        return e;
    take_entry(m, entry);
    return 0;
}
