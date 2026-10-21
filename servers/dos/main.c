/*
 * servers/dos/main.c -- dos: files, by path, on the FAT32 volumes of every
 * block device (planar/dos.h, v4-plan step 6)
 *
 * At start it opens each block port it finds ("nvme.N", "sd.N") and
 * mounts each GPT partition that holds FAT32 (fat.c). Then it serves
 * "dos", one task, one message at a time. Reads and writes go through
 * lib/blk, which reconnects to a driver that restarted: a volume's files
 * stay open across it.
 */
#include <stddef.h>

#include <planar/blk.h>
#include <planar/dos.h>
#include <planar/syscalls.h>

#include "fat.h"

void *memset(void *dst, int c, size_t n);
void *memcpy(void *restrict dst, const void *restrict src, size_t n);

constexpr int64_t SIG_PORT = 16;            /* a client at "dos" */
constexpr int64_t SIG_CONN = 17;            /* a client's connection */
constexpr int64_t SIG_BLK = 18;             /* the block devices' connections */
constexpr unsigned DEVICES = 8;
constexpr unsigned VOLUMES = 8;
constexpr unsigned PARTITIONS = 8;          /* looked at per device */
constexpr unsigned CLIENTS = 16;
constexpr unsigned FILES = 64;

struct volume {
    char name[16];
    struct blk *dev;
    uint64_t first, count;          /* blocks on the device */
    struct fat_vol v;
    uint8_t *cache;
};

struct file {
    bool used;
    bool writable;
    unsigned client;
    struct volume *vol;
    struct fat_node node;
};

static struct blk devs[DEVICES];
static unsigned ndevs;
static struct volume vols[VOLUMES];
static unsigned nvols;
static int sys_vol = -1;            /* the volume holding EFI/planar */
static struct MsgPort clients[CLIENTS];
static unsigned client_id[CLIENTS], next_id = 1;
static unsigned nclients;
static struct file files[FILES];

static void say(const char *s)
{
    KLog(s);
}

static unsigned len(const char *s)
{
    unsigned n = 0;

    while (s[n])
        n++;
    return n;
}

static void cat(char *dst, unsigned size, const char *src)
{
    unsigned n = len(dst);

    while (*src && n < size - 1)
        dst[n++] = *src++;
    dst[n] = '\0';
}

static void cat_dec(char *dst, unsigned size, uint64_t v)
{
    char d[24];
    unsigned k = 0, n = len(dst);

    do
        d[k++] = (char)('0' + v % 10);
    while ((v /= 10) && k < sizeof d);
    while (k && n < size - 1)
        dst[n++] = d[--k];
    dst[n] = '\0';
}

static bool same(const char *a, const char *b, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        char x = a[i] >= 'A' && a[i] <= 'Z' ? (char)(a[i] + 32) : a[i];
        char y = b[i] >= 'A' && b[i] <= 'Z' ? (char)(b[i] + 32) : b[i];

        if (x != y)
            return false;
        if (!x)
            return true;            /* both ended */
    }
    return true;
}

/* --- Volumes. --- */

static int64_t vol_read(void *ctx, uint64_t s, uint32_t n, void *buf)
{
    struct volume *vol = ctx;

    if (s >= vol->count || n > vol->count - s)
        return ERR_BADARG;
    return blk_read(vol->dev, vol->first + s, n, buf);
}

static int64_t vol_write(void *ctx, uint64_t s, uint32_t n, const void *buf)
{
    struct volume *vol = ctx;

    if (s >= vol->count || n > vol->count - s)
        return ERR_BADARG;
    return blk_write(vol->dev, vol->first + s, n, buf);
}

static void mount_all(void)
{
    static const char *const kinds[] = { "nvme.", "sd." };

    for (unsigned k = 0; k < 2; k++)
        for (unsigned u = 0; u < 4 && ndevs < DEVICES; u++) {
            char port[16] = "";
            struct blk *b = &devs[ndevs];

            cat(port, sizeof port, kinds[k]);
            cat_dec(port, sizeof port, u);
            if (blk_open_now(b, port, SIG_BLK, 256 * 1024) != 0)
                continue;
            ndevs++;
            for (unsigned p = 1; p <= PARTITIONS && nvols < VOLUMES; p++) {
                struct volume *vol = &vols[nvols];
                struct fat_io io = { vol, vol_read, vol_write };
                char line[96] = "dos: ";
                void *mem;

                if (blk_partition_range(b, p, &vol->first, &vol->count) != 0)
                    break;
                if (!vol->cache) {
                    if (AllocArea(FAT_MEM, 0, &mem) < 0)
                        return;
                    vol->cache = mem;
                }
                vol->dev = b;
                if (b->block_size != 512 || fat_mount(&vol->v, &io, vol->count, vol->cache) != 0)
                    continue;           /* not FAT32: another system's, left alone */
                vol->name[0] = '\0';
                bool taken = false;
                for (unsigned i = 0; i < nvols; i++)
                    taken |= len(vols[i].name) == len(vol->v.label) &&
                             same(vols[i].name, vol->v.label, len(vol->v.label));
                if (vol->v.label[0] && !same(vol->v.label, "no name", 8) && !taken) {
                    cat(vol->name, sizeof vol->name, vol->v.label);
                } else {
                    cat(vol->name, sizeof vol->name, k ? "sd" : "nvme");
                    cat_dec(vol->name, sizeof vol->name, u);
                    cat(vol->name, sizeof vol->name, "p");
                    cat_dec(vol->name, sizeof vol->name, p);
                }
                vol->v.read_only = false;
                nvols++;
                {
                    struct fat_node n;

                    if (sys_vol < 0 && fat_lookup(&vol->v, "EFI/planar", &n) == 0 && n.dir)
                        sys_vol = (int)(vol - vols);
                }
                cat(line, sizeof line, vol->name);
                cat(line, sizeof line, ": FAT32 on ");
                cat(line, sizeof line, port);
                cat(line, sizeof line, " partition ");
                cat_dec(line, sizeof line, p);
                cat(line, sizeof line, vol->v.read_only ? ", read only" : ", writable");
                if (sys_vol == (int)(vol - vols))
                    cat(line, sizeof line, "; SYS:");
                say(line);
            }
        }
}

/* "VOLUME:path", or an assign's (SYS:, C:): the volume, and the path on it. */
static struct volume *resolve(const char *in, char *path, unsigned size)
{
    unsigned colon = 0;

    while (in[colon] && in[colon] != ':')
        colon++;
    if (!in[colon] || colon == 0)
        return nullptr;
    path[0] = '\0';
    if ((colon == 3 && same(in, "sys", 3)) || (colon == 1 && same(in, "c", 1))) {
        if (sys_vol < 0)
            return nullptr;
        if (colon == 1)
            cat(path, size, "EFI/planar/C/");
        cat(path, size, in + colon + 1);
        return &vols[sys_vol];
    }
    for (unsigned i = 0; i < nvols; i++)
        if (len(vols[i].name) == colon && same(vols[i].name, in, colon)) {
            cat(path, size, in + colon + 1);
            return &vols[i];
        }
    return nullptr;
}

static void entry_of(const struct fat_node *n, struct DosEntry *e)
{
    memset(e, 0, sizeof *e);
    memcpy(e->de_Name, n->name, sizeof e->de_Name < FAT_NAME ? sizeof e->de_Name : FAT_NAME);
    e->de_Name[sizeof e->de_Name - 1] = '\0';
    e->de_Size = n->size;
    e->de_Dir = n->dir;
    e->de_Date = n->date;
    e->de_Time = n->time;
}

/* --- The protocol. --- */

static struct file *file_of(unsigned client, uint32_t f)
{
    return f < FILES && files[f].used && files[f].client == client ? &files[f] : nullptr;
}

static bool in_heap(const struct MsgPort *mp, uint64_t a, uint64_t n)
{
    return a >= mp->mp_HeapLo && a <= mp->mp_HeapHi && n <= mp->mp_HeapHi - a;
}

static int64_t serve_one(unsigned c, struct DosMsg *m)
{
    struct DosMsg q;                /* the client's: copied once */
    char path[DOS_PATH];
    struct volume *vol;
    struct file *f;
    int64_t e;

    memcpy(&q, m, sizeof q);
    q.dm_Path[DOS_PATH - 1] = '\0';
    if (q.dm_Version != DOS_VERSION)
        return ERR_BADARG;
    switch (q.dm_Op) {
    case DOS_OPEN: {
        struct fat_node n;
        unsigned i = 0;

        if (!(vol = resolve(q.dm_Path, path, sizeof path)))
            return ERR_NOTFOUND;
        if ((q.dm_Flags & DOSF_WRITE) && vol->v.read_only)
            return ERR_RIGHTS;
        e = fat_lookup(&vol->v, path, &n);
        if (e == ERR_NOTFOUND && (q.dm_Flags & DOSF_CREATE))
            e = vol->v.read_only ? ERR_RIGHTS : fat_create(&vol->v, path, false, &n);
        if (e)
            return e;
        while (i < FILES && files[i].used)
            i++;
        if (i == FILES)
            return ERR_LIMIT;
        files[i] = (struct file){ .used = true, .writable = q.dm_Flags & DOSF_WRITE,
                                  .client = client_id[c], .vol = vol, .node = n };
        m->dm_File = i;
        entry_of(&n, &m->dm_Entry);
        return 0;
    }
    case DOS_CLOSE:
        if (!(f = file_of(client_id[c], q.dm_File)))
            return ERR_BADHANDLE;
        f->used = false;
        return 0;
    case DOS_READ:
    case DOS_WRITE:
        if (!(f = file_of(client_id[c], q.dm_File)))
            return ERR_BADHANDLE;
        if (q.dm_Length > DOS_MAX_IO || !in_heap(&clients[c], q.dm_Buffer, q.dm_Length))
            return ERR_BADARG;
        if (q.dm_Op == DOS_WRITE && !f->writable)
            return ERR_RIGHTS;
        e = q.dm_Op == DOS_READ
                ? fat_read(&f->vol->v, &f->node, q.dm_Offset, (void *)q.dm_Buffer, q.dm_Length)
                : fat_write(&f->vol->v, &f->node, q.dm_Offset, (const void *)q.dm_Buffer,
                            q.dm_Length);
        if (e < 0)
            return e;
        m->dm_Length = (uint32_t)e;
        return 0;
    case DOS_TRUNCATE:
        if (!(f = file_of(client_id[c], q.dm_File)))
            return ERR_BADHANDLE;
        if (!f->writable)
            return ERR_RIGHTS;
        return fat_truncate(&f->vol->v, &f->node, q.dm_Offset > UINT32_MAX ? UINT32_MAX
                                                                           : (uint32_t)q.dm_Offset);
    case DOS_STAT: {
        struct fat_node n;

        if (!(vol = resolve(q.dm_Path, path, sizeof path)))
            return ERR_NOTFOUND;
        if ((e = fat_lookup(&vol->v, path, &n)))
            return e;
        entry_of(&n, &m->dm_Entry);
        return 0;
    }
    case DOS_NEXT: {
        struct fat_node n;
        uint32_t cursor = q.dm_Cursor;

        if (!(f = file_of(client_id[c], q.dm_File)))
            return ERR_BADHANDLE;
        if ((e = fat_next(&f->vol->v, &f->node, &cursor, &n)))
            return e;
        entry_of(&n, &m->dm_Entry);
        m->dm_Cursor = cursor;
        return 0;
    }
    case DOS_REMOVE:
    case DOS_MKDIR: {
        struct fat_node n;

        if (!(vol = resolve(q.dm_Path, path, sizeof path)))
            return ERR_NOTFOUND;
        if (vol->v.read_only)
            return ERR_RIGHTS;
        if (q.dm_Op == DOS_REMOVE)
            for (unsigned i = 0; i < FILES; i++)
                if (files[i].used && files[i].vol == vol &&
                    fat_lookup(&vol->v, path, &n) == 0 &&
                    files[i].node.entry_dir == n.entry_dir && files[i].node.entry_pos == n.entry_pos)
                    return ERR_BUSY;    /* open */
        return q.dm_Op == DOS_REMOVE ? fat_remove(&vol->v, path) : fat_create(&vol->v, path, true, &n);
    }
    case DOS_VOLUME:
        if (q.dm_Cursor >= nvols)
            return ERR_NOTFOUND;
        vol = &vols[q.dm_Cursor];
        memset(&m->dm_Entry, 0, sizeof m->dm_Entry);
        memcpy(m->dm_Entry.de_Name, vol->name, sizeof vol->name);
        m->dm_Entry.de_Size = (uint64_t)fat_free(&vol->v) * vol->v.spc * vol->v.bps;
        m->dm_Entry.de_Dir = !vol->v.read_only;
        return 0;
    default:
        return ERR_BADARG;
    }
}

static void serve(unsigned c)
{
    struct DosMsg *m;

    while ((m = (struct DosMsg *)GetMsgSized(&clients[c], sizeof *m))) {
        m->dm_Msg.mn_Error = (int32_t)serve_one(c, m);
        ReplyMsgLater(&clients[c], &m->dm_Msg);
    }
}

int main(void)
{
    int64_t port;

    mount_all();
    if (nvols == 0)
        say("dos: no FAT32 volume on any block device");
    if ((port = CreatePort("dos", SIG_PORT)) < 0) {
        say("dos: no port");
        return 1;
    }
    for (;;) {
        uint64_t got = WaitPorts(clients, nclients, 1ULL << SIG_PORT | 1ULL << SIG_CONN);

        if (got & 1ULL << SIG_PORT)
            for (;;) {
                struct MsgPort mp;

                if (AcceptPort(&mp, (Handle)port, SIG_CONN) != 0)
                    break;
                if (nclients == CLIENTS) {
                    ClosePort(&mp);
                    continue;
                }
                client_id[nclients] = next_id++;
                clients[nclients++] = mp;
            }
        for (unsigned c = 0; c < nclients;) {
            serve(c);
            if (!PortGone(&clients[c])) {
                c++;
                continue;
            }
            for (unsigned i = 0; i < FILES; i++)    /* its files close with it */
                if (files[i].used && files[i].client == client_id[c])
                    files[i].used = false;
            ClosePort(&clients[c]);
            clients[c] = clients[--nclients];
            client_id[c] = client_id[nclients];
        }
    }
}
