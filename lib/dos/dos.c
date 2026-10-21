/*
 * lib/dos/dos.c -- dos.library: the dos server's client, and the loader
 * (planar/dos.h)
 */
#include <stddef.h>

#include <planar/dos.h>
#include <planar/drv.h>
#include <planar/syscalls.h>

void *memset(void *dst, int c, size_t n);
void *memcpy(void *restrict dst, const void *restrict src, size_t n);

constexpr uint64_t PAGE = 4096;
constexpr uint64_t IMAGE_MAX = 64ULL << 20;     /* a program's whole image, at most */

int64_t dos_open(struct dos *d, int64_t sigbit)
{
    int64_t e;

    memset(d, 0, sizeof *d);
    if ((e = ConnectPort(&d->mp, "dos", sigbit, 4, sizeof *d->m + DOS_MAX_IO + 64)) != 0)
        return e;
    d->open = true;
    if (!(d->m = AllocMsg(&d->mp, sizeof *d->m)) || !(d->buf = AllocMsg(&d->mp, DOS_MAX_IO))) {
        dos_close(d);
        return ERR_NOMEM;
    }
    return 0;
}

void dos_close(struct dos *d)
{
    if (d->open)
        ClosePort(&d->mp);
    d->open = false;
}

static struct DosMsg *msg(struct dos *d, uint32_t op)
{
    memset(d->m, 0, offsetof(struct DosMsg, dm_Path));
    d->m->dm_Msg.mn_Length = sizeof *d->m;
    d->m->dm_Version = DOS_VERSION;
    d->m->dm_Op = op;
    d->m->dm_Path[0] = '\0';
    return d->m;
}

static void set_path(struct DosMsg *m, const char *path)
{
    unsigned i = 0;

    for (; path[i] && i < DOS_PATH - 1; i++)
        m->dm_Path[i] = path[i];
    m->dm_Path[i] = '\0';
}

static int64_t call(struct dos *d)
{
    int64_t e;

    if (!d->open)
        return ERR_PORT_DIED;
    e = DoMsg(&d->mp, &d->m->dm_Msg);
    return e ? e : d->m->dm_Msg.mn_Error;
}

static void take_entry(const struct DosMsg *m, struct DosEntry *e)
{
    if (e) {
        memcpy(e, &m->dm_Entry, sizeof *e);
        e->de_Name[DOS_PATH - 1] = '\0';
    }
}

int64_t dos_file(struct dos *d, const char *path, uint32_t flags, struct DosEntry *entry)
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

int64_t dos_close_file(struct dos *d, uint32_t file)
{
    msg(d, DOS_CLOSE)->dm_File = file;
    return call(d);
}

int64_t dos_read(struct dos *d, uint32_t file, uint64_t off, void *buf, uint32_t len)
{
    uint32_t done = 0;

    while (done < len) {
        struct DosMsg *m = msg(d, DOS_READ);
        uint32_t n = len - done < DOS_MAX_IO ? len - done : DOS_MAX_IO;
        int64_t e;

        m->dm_File = file;
        m->dm_Offset = off + done;
        m->dm_Length = n;
        m->dm_Buffer = (uint64_t)d->buf;
        if ((e = call(d)))
            return e;
        if (m->dm_Length > n)
            return ERR_BADARG;      /* the server's answer, checked */
        memcpy((uint8_t *)buf + done, d->buf, m->dm_Length);
        done += m->dm_Length;
        if (m->dm_Length < n)
            break;                  /* the end of the file */
    }
    return done;
}

int64_t dos_write(struct dos *d, uint32_t file, uint64_t off, const void *buf, uint32_t len)
{
    uint32_t done = 0;

    while (done < len) {
        struct DosMsg *m = msg(d, DOS_WRITE);
        uint32_t n = len - done < DOS_MAX_IO ? len - done : DOS_MAX_IO;
        int64_t e;

        memcpy(d->buf, (const uint8_t *)buf + done, n);
        m->dm_File = file;
        m->dm_Offset = off + done;
        m->dm_Length = n;
        m->dm_Buffer = (uint64_t)d->buf;
        if ((e = call(d)))
            return e;
        if (m->dm_Length != n)
            return ERR_FULL;
        done += n;
    }
    return done;
}

int64_t dos_stat(struct dos *d, const char *path, struct DosEntry *entry)
{
    struct DosMsg *m = msg(d, DOS_STAT);
    int64_t e;

    set_path(m, path);
    if ((e = call(d)))
        return e;
    take_entry(m, entry);
    return 0;
}

int64_t dos_next(struct dos *d, uint32_t dir, uint32_t *cursor, struct DosEntry *entry)
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

int64_t dos_remove(struct dos *d, const char *path)
{
    set_path(msg(d, DOS_REMOVE), path);
    return call(d);
}

int64_t dos_mkdir(struct dos *d, const char *path)
{
    set_path(msg(d, DOS_MKDIR), path);
    return call(d);
}

int64_t dos_volume(struct dos *d, uint32_t index, struct DosEntry *entry)
{
    struct DosMsg *m = msg(d, DOS_VOLUME);
    int64_t e;

    m->dm_Cursor = index;
    if ((e = call(d)))
        return e;
    take_entry(m, entry);
    return 0;
}

/* --- The loader (ELF-64, System V gABI). --- */

extern const uint16_t user_elf_machine;     /* arch/<arch>/user/elf.c */

static uint64_t le(const uint8_t *p, unsigned n)
{
    uint64_t v = 0;

    for (unsigned i = 0; i < n; i++)
        v |= (uint64_t)p[i] << 8 * i;
    return v;
}

int64_t dos_run(struct dos *d, const char *path, const char *args, int64_t sigbit, uint32_t flags)
{
    uint64_t arg = 0;
    uint8_t eh[64], ph[16 * 56];
    struct ProcessImage pi = { 0 };
    struct DosEntry de;
    uint64_t total = 0, etext = 0, phoff, entry;
    uint16_t phnum, phentsize;
    int64_t f, e, area;
    uint8_t *image;
    const char *base = path;

    if ((f = dos_file(d, path, 0, &de)) < 0)
        return f;
    e = ERR_BADARG;
    if (dos_read(d, (uint32_t)f, 0, eh, sizeof eh) != sizeof eh || eh[0] != 0x7f || eh[1] != 'E' ||
        eh[2] != 'L' || eh[3] != 'F' || eh[4] != 2 || eh[5] != 1 || le(eh + 16, 2) != 3 ||
        le(eh + 18, 2) != user_elf_machine)
        goto out;                   /* not a 64-bit little-endian PIE for this machine */
    entry = le(eh + 24, 8);
    phoff = le(eh + 32, 8);
    phentsize = (uint16_t)le(eh + 54, 2);
    phnum = (uint16_t)le(eh + 56, 2);
    if (phentsize != 56 || phnum == 0 || phnum > 16 || phoff > de.de_Size ||
        (uint64_t)phnum * 56 > de.de_Size - phoff ||
        dos_read(d, (uint32_t)f, phoff, ph, phnum * 56u) != phnum * 56)
        goto out;
    /* Its size, and where the code ends: the first writable segment. */
    for (unsigned i = 0; i < phnum; i++) {
        const uint8_t *p = ph + 56 * i;
        uint64_t va = le(p + 16, 8), filesz = le(p + 32, 8), memsz = le(p + 40, 8);

        if (le(p, 4) != 1)          /* PT_LOAD */
            continue;
        if (memsz < filesz || va > IMAGE_MAX || memsz > IMAGE_MAX - va ||
            le(p + 8, 8) > de.de_Size || filesz > de.de_Size - le(p + 8, 8))
            goto out;
        if (va + memsz > total)
            total = va + memsz;
        if (le(p + 4, 4) & 2 && (!etext || va < etext))    /* PF_W */
            etext = va;
    }
    total = (total + PAGE - 1) & ~(PAGE - 1);
    if (!etext)
        etext = total;
    if (total == 0 || etext % PAGE || entry >= etext)
        goto out;
    if ((area = AllocArea(total + PAGE, AREAF_EXEC, (void **)&image)) < 0) {
        e = area;
        goto out;
    }
    /* The command line, in the page past the image. */
    if (args) {
        char *line = (char *)image + total;
        unsigned i = 0;

        for (; args[i] && i < DOS_ARGS - 1; i++)
            line[i] = args[i];
        line[i] = '\0';
        arg = (uint64_t)line;
    }
    for (unsigned i = 0; i < phnum; i++) {
        const uint8_t *p = ph + 56 * i;
        uint64_t filesz = le(p + 32, 8);

        if (le(p, 4) != 1 || filesz == 0)
            continue;
        if (dos_read(d, (uint32_t)f, le(p + 8, 8), image + le(p + 16, 8), (uint32_t)filesz) !=
            (int64_t)filesz) {
            FreeArea((Handle)area);
            goto out;
        }
    }
    /* Its name: the path's last part. */
    for (const char *s = path; *s; s++)
        if (*s == '/' || *s == ':')
            base = s + 1;
    for (unsigned i = 0; i < sizeof pi.pi_Name - 1 && base[i]; i++)
        pi.pi_Name[i] = base[i];
    pi.pi_Area = (Handle)area;
    pi.pi_Etext = etext;
    pi.pi_Entry = entry;
    e = CreateProcessImage(&pi, arg, sigbit, flags);
    if (e < 0)
        FreeArea((Handle)area);     /* still ours if the kernel didn't take it */
out:
    dos_close_file(d, (uint32_t)f);
    return e;
}

void dos_args(uint64_t arg, char *cwd, unsigned cwd_size, const char **args)
{
    const char *line = arg ? (const char *)arg : "";
    unsigned n = 0;

    *args = line;
    cwd[0] = '\0';
    for (const char *p = line; *p; p++)
        if (*p == '\n') {
            for (const char *q = line; q < p && n < cwd_size - 1; q++)
                cwd[n++] = *q;
            cwd[n] = '\0';
            *args = p + 1;
            break;
        }
    if (!cwd[0] && cwd_size > 4) {
        cwd[0] = 'S', cwd[1] = 'Y', cwd[2] = 'S', cwd[3] = ':', cwd[4] = '\0';
    }
}

void dos_path(const char *cwd, const char *name, char *out, unsigned size)
{
    unsigned n = 0;
    bool volume = false;

    for (const char *p = name; *p; p++)
        volume |= *p == ':';
    if (!volume) {
        for (; *cwd && n < size - 1; cwd++)
            out[n++] = *cwd;
        if (n && out[n - 1] != ':' && out[n - 1] != '/' && n < size - 1 && *name)
            out[n++] = '/';
    }
    for (; *name && n < size - 1; name++)
        out[n++] = *name;
    out[n] = '\0';
}

/* --- The console. --- */

void con_open(struct con *c, int64_t sigbit)
{
    memset(c, 0, sizeof *c);
    c->sigbit = sigbit;
    if (ConnectPort(&c->mp, "con.0", sigbit, 4, 2 * sizeof(struct DrvRequest)) != 0)
        return;
    if (!(c->m = AllocMsg(&c->mp, sizeof(struct DrvRequest)))) {
        ClosePort(&c->mp);
        return;
    }
    c->open = true;
}

void con_close(struct con *c)
{
    if (c->nline) {
        c->line[c->nline] = '\0';
        KLog(c->line);
        c->nline = 0;
    }
    if (c->open)
        ClosePort(&c->mp);
    c->open = false;
}

void con_write(struct con *c, const char *text, uint32_t len)
{
    while (len) {
        struct DrvRequest *r = c->m;
        uint32_t n = len < sizeof r->dr_Data ? len : sizeof r->dr_Data;

        if (c->open) {
            memset(r, 0, sizeof *r);
            r->dr_Msg.mn_Length = sizeof *r;
            r->dr_Op = DRV_WRITE;
            r->dr_Len = n;
            memcpy(r->dr_Data, text, n);
            if (DoMsg(&c->mp, &r->dr_Msg) == 0 && r->dr_Msg.mn_Error == 0) {
                text += n;
                len -= n;
                continue;
            }
            /* The console went (display restarting): once more on a new
             * connection, else to the log from here. */
            ClosePort(&c->mp);
            c->open = false;
            if (!c->retried) {
                c->retried = true;
                con_open(c, c->sigbit);
                c->retried = true;
                if (c->open)
                    continue;
            }
        }
        for (uint32_t i = 0; i < n; i++) {
            if (text[i] == '\n' || c->nline == sizeof c->line - 1) {
                c->line[c->nline] = '\0';
                KLog(c->line);
                c->nline = 0;
                if (text[i] == '\n')
                    continue;
            }
            c->line[c->nline++] = text[i];
        }
        text += n;
        len -= n;
    }
}

void con_puts(struct con *c, const char *text)
{
    uint32_t n = 0;

    while (text[n])
        n++;
    con_write(c, text, n);
}

void con_dec(struct con *c, uint64_t v)
{
    char d[24];
    unsigned n = 0, k = 0;
    char out[24];

    do
        d[n++] = (char)('0' + v % 10);
    while ((v /= 10) && n < sizeof d);
    while (n)
        out[k++] = d[--n];
    con_write(c, out, k);
}
