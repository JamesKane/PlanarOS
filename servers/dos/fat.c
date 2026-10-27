/*
 * servers/dos/fat.c -- FAT32 volumes (fat.h)
 */
#include <planar/endian.h>
#include <planar/syscalls.h>

#include "fat.h"

constexpr uint32_t EOC = 0x0ffffff8;        /* this and above: the chain's end */
constexpr uint32_t BAD = 0x0ffffff7;
constexpr uint8_t ATTR_VOLUME = 0x08, ATTR_DIR = 0x10, ATTR_ARCHIVE = 0x20;
constexpr uint8_t ATTR_LFN = 0x0f;
constexpr uint8_t NT_LOWER_BASE = 0x08, NT_LOWER_EXT = 0x10;
constexpr uint16_t DATE_2026 = (2026 - 1980) << 9 | 1 << 5 | 1;   /* no clock here yet */

static void put16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    put16(p, v);
    put16(p + 2, v >> 16);
}

static void copy(void *dst, const void *src, uint32_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;

    while (n--)
        *d++ = *s++;
}

static void fill(void *dst, uint8_t c, uint32_t n)
{
    uint8_t *d = dst;

    while (n--)
        *d++ = c;
}

static char lower(char c)
{
    return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c;
}

static char upper(char c)
{
    return c >= 'a' && c <= 'z' ? (char)(c - 32) : c;
}

/* --- Sectors: the cache, write-through. --- */

static int64_t sector(struct fat_vol *v, uint64_t s, uint8_t **out)
{
    unsigned oldest = 0;

    if (s >= v->sectors)
        return ERR_BADARG;
    for (unsigned i = 0; i < FAT_CACHE; i++) {
        if (v->cache[i].valid && v->cache[i].sector == s) {
            v->cache[i].used = ++v->tick;
            *out = v->cache_mem + (uint64_t)i * v->bps;
            return 0;
        }
        if (!v->cache[i].valid || v->cache[i].used < v->cache[oldest].used)
            oldest = i;
    }
    *out = v->cache_mem + (uint64_t)oldest * v->bps;
    v->cache[oldest].valid = false;
    int64_t e = v->io.read(v->io.ctx, s, 1, *out);
    if (e)
        return e;
    v->cache[oldest] = (typeof(v->cache[0])){ s, ++v->tick, true };
    return 0;
}

/* The cached copy of sector s, changed by the caller, to the disk. */
static int64_t store(struct fat_vol *v, uint64_t s)
{
    for (unsigned i = 0; i < FAT_CACHE; i++)
        if (v->cache[i].valid && v->cache[i].sector == s)
            return v->io.write(v->io.ctx, s, 1, v->cache_mem + (uint64_t)i * v->bps);
    return ERR_BADARG;
}

static void forget(struct fat_vol *v, uint64_t s, uint32_t n)
{
    for (unsigned i = 0; i < FAT_CACHE; i++)
        if (v->cache[i].valid && v->cache[i].sector >= s && v->cache[i].sector - s < n)
            v->cache[i].valid = false;
}

/* --- Clusters and the FAT. --- */

static bool valid(const struct fat_vol *v, uint32_t c)
{
    return c >= 2 && c - 2 < v->clusters;
}

static uint64_t first_sector(const struct fat_vol *v, uint32_t c)
{
    return v->data_start + (uint64_t)(c - 2) * v->spc;
}

static uint32_t cluster_bytes(const struct fat_vol *v)
{
    return v->spc * v->bps;
}

static int64_t fat_get(struct fat_vol *v, uint32_t c, uint32_t *out)
{
    uint8_t *p;
    uint64_t off = (uint64_t)c * 4;
    int64_t e = sector(v, v->fat_start + off / v->bps, &p);

    if (e)
        return e;
    *out = get_le32(p + off % v->bps) & 0x0fffffff;
    return 0;
}

static int64_t mark_fsinfo_stale(struct fat_vol *v)
{
    uint8_t *p;
    int64_t e;

    if (!v->fsinfo || v->fsinfo_stale)
        return 0;
    if ((e = sector(v, v->fsinfo, &p)))
        return e;
    if (get_le32(p) == 0x41615252 && get_le32(p + 484) == 0x61417272) {
        put32(p + 488, 0xffffffff);     /* free count unknown: whoever mounts next counts */
        if ((e = store(v, v->fsinfo)))
            return e;
    }
    v->fsinfo_stale = true;
    return 0;
}

/* Set FAT entry c in every copy, keeping its top four bits. */
static int64_t fat_set(struct fat_vol *v, uint32_t c, uint32_t val)
{
    uint64_t off = (uint64_t)c * 4;
    int64_t e;

    if (v->read_only)
        return ERR_RIGHTS;
    if ((e = mark_fsinfo_stale(v)))
        return e;
    v->walked.first = 0;            /* a chain may have changed */
    for (uint32_t f = 0; f < v->nfats; f++) {
        uint64_t s = v->fat_start + (uint64_t)f * v->fat_size + off / v->bps;
        uint8_t *p;

        if ((e = sector(v, s, &p)))
            return e;
        put32(p + off % v->bps, (get_le32(p + off % v->bps) & 0xf0000000) | (val & 0x0fffffff));
        if ((e = store(v, s)))
            return e;
    }
    return 0;
}

/* The cluster after c in its chain; 0 at the end. An error for a chain
 * that leads nowhere valid. */
static int64_t next_cluster(struct fat_vol *v, uint32_t c, uint32_t *out)
{
    uint32_t n;
    int64_t e = fat_get(v, c, &n);

    if (e)
        return e;
    if (n >= EOC) {
        *out = 0;
        return 0;
    }
    if (!valid(v, n) || n == BAD)
        return ERR_BADARG;
    *out = n;
    return 0;
}

/* The k-th cluster of the chain from 'first' (k from 0); 0 if it's
 * shorter. Bounded, so a chain that loops ends. */
static int64_t nth_cluster(struct fat_vol *v, uint32_t first, uint64_t k, uint32_t *out)
{
    uint32_t c = first;
    uint64_t i = 0;

    if (!valid(v, first))
        return ERR_BADARG;
    if (v->walked.first == first && v->walked.k <= k) {
        i = v->walked.k;
        c = v->walked.c;
    }
    for (; i < k; i++) {
        int64_t e;

        if (i > v->clusters)
            return ERR_BADARG;      /* a loop */
        if ((e = next_cluster(v, c, &c)))
            return e;
        if (!c)
            break;
    }
    if (c)
        v->walked = (typeof(v->walked)){ first, (uint32_t)k, c };
    *out = c;
    return 0;
}

static int64_t zero_cluster(struct fat_vol *v, uint32_t c)
{
    uint64_t s = first_sector(v, c);

    fill(v->scratch, 0, v->bps);
    forget(v, s, v->spc);
    for (uint32_t i = 0; i < v->spc; i++) {
        int64_t e = v->io.write(v->io.ctx, s + i, 1, v->scratch);

        if (e)
            return e;
    }
    return 0;
}

/* A free cluster, marked as a chain's end and zeroed, linked after
 * 'prev' if that's not 0. */
static int64_t alloc_cluster(struct fat_vol *v, uint32_t prev, uint32_t *out)
{
    for (uint32_t i = 0; i < v->clusters; i++) {
        uint32_t c = 2 + (v->next_free - 2 + i) % v->clusters, val;
        int64_t e;

        if ((e = fat_get(v, c, &val)))
            return e;
        if (val)
            continue;
        if ((e = fat_set(v, c, 0x0fffffff)) || (e = zero_cluster(v, c)))
            return e;
        if (prev && (e = fat_set(v, prev, c)))
            return e;
        if (v->free != UINT32_MAX)
            v->free--;
        v->next_free = c + 1 < v->clusters + 2 ? c + 1 : 2;
        *out = c;
        return 0;
    }
    return ERR_FULL;
}

/* Free the chain from c on. */
static int64_t free_chain(struct fat_vol *v, uint32_t c)
{
    for (uint32_t i = 0; c && i <= v->clusters; i++) {
        uint32_t n;
        int64_t e;

        if (!valid(v, c))
            return ERR_BADARG;
        if ((e = next_cluster(v, c, &n)) || (e = fat_set(v, c, 0)))
            return e;
        if (v->free != UINT32_MAX)
            v->free++;
        c = n;
    }
    return 0;
}

/* --- Mounting. --- */

int64_t fat_mount(struct fat_vol *v, const struct fat_io *io, uint64_t sectors, void *cache)
{
    uint8_t *b = cache;
    uint32_t bps, spc, reserved, nfats, fat_size, total, data;
    int64_t e;

    *v = (struct fat_vol){ .io = *io, .sectors = sectors, .cache_mem = cache };
    if (sectors < 64 || (e = io->read(io->ctx, 0, 1, b)))
        return sectors < 64 ? ERR_NOTFOUND : e;
    bps = get_le16(b + 11);
    spc = b[13];
    reserved = get_le16(b + 14);
    nfats = b[16];
    total = get_le16(b + 19) ? get_le16(b + 19) : get_le32(b + 32);
    fat_size = get_le16(b + 22) ? get_le16(b + 22) : get_le32(b + 36);
    if (b[510] != 0x55 || b[511] != 0xaa || bps < 512 || bps > 4096 || (bps & (bps - 1)) ||
        spc == 0 || (spc & (spc - 1)) || reserved == 0 || nfats == 0 || nfats > 4 ||
        get_le16(b + 17) != 0 || get_le16(b + 22) != 0 || fat_size == 0)
        return ERR_NOTFOUND;            /* not FAT32: FAT12 and 16 have root entries */
    if (total > sectors || (uint64_t)reserved + (uint64_t)nfats * fat_size >= total)
        return ERR_BADARG;
    data = reserved + nfats * fat_size;
    v->bps = bps;
    v->spc = spc;
    v->fat_start = reserved;
    v->fat_size = fat_size;
    v->nfats = nfats;
    v->data_start = data;
    v->sectors = total;
    v->clusters = (total - data) / spc;
    if (v->clusters < 65525 || (uint64_t)(v->clusters + 2) * 4 > (uint64_t)fat_size * bps)
        return ERR_NOTFOUND;            /* too few clusters for FAT32, or a FAT too small */
    v->root = get_le32(b + 44);
    if (!valid(v, v->root))
        return ERR_BADARG;
    v->fsinfo = get_le16(b + 48);
    if (v->fsinfo == 0 || v->fsinfo == 0xffff || v->fsinfo >= reserved)
        v->fsinfo = 0;
    v->next_free = 2;
    v->free = UINT32_MAX;
    /* FSInfo's free count, if it's there and plausible: else counted when asked. */
    if (v->fsinfo && io->read(io->ctx, v->fsinfo, 1, b) == 0 && get_le32(b) == 0x41615252 &&
        get_le32(b + 484) == 0x61417272 && get_le32(b + 488) <= v->clusters)
        v->free = get_le32(b + 488);
    v->scratch = v->cache_mem + (uint64_t)FAT_CACHE * bps;
    for (unsigned i = 0; i < 11; i++)
        v->label[i] = (char)(b[71 + i] >= 32 && b[71 + i] < 127 ? b[71 + i] : '_');
    for (int i = 10; i >= 0 && v->label[i] == ' '; i--)
        v->label[i] = '\0';
    v->label[11] = '\0';
    return 0;
}

/* --- Directories. --- */

/* Entry 'pos' of the directory starting at cluster 'dir': its sector and
 * byte offset there. ERR_NOTFOUND past the chain's end. */
static int64_t entry_at(struct fat_vol *v, uint32_t dir, uint32_t pos, uint64_t *sec, uint32_t *off)
{
    uint32_t per = cluster_bytes(v) / 32, c;
    int64_t e = nth_cluster(v, dir, pos / per, &c);

    if (e)
        return e;
    if (!c)
        return ERR_NOTFOUND;
    *sec = first_sector(v, c) + (uint64_t)(pos % per) * 32 / v->bps;
    *off = (pos % per) * 32 % v->bps;
    return 0;
}

/* A short name's 11 bytes as text, lower case where its flags say. */
static void short_name(const uint8_t *d, char *out)
{
    unsigned n = 0;
    bool lb = d[12] & NT_LOWER_BASE, le = d[12] & NT_LOWER_EXT;

    for (unsigned i = 0; i < 8 && d[i] != ' '; i++) {
        char c = (char)(i == 0 && d[0] == 0x05 ? 0xe5 : d[i]);

        out[n++] = lb ? lower(c) : c;
    }
    if (d[8] != ' ') {
        out[n++] = '.';
        for (unsigned i = 8; i < 11 && d[i] != ' '; i++)
            out[n++] = le ? lower((char)d[i]) : (char)d[i];
    }
    out[n] = '\0';
}

static uint8_t checksum(const uint8_t *name)
{
    uint8_t sum = 0;

    for (unsigned i = 0; i < 11; i++)
        sum = (uint8_t)(((sum & 1) << 7) + (sum >> 1) + name[i]);
    return sum;
}

/* UTF-8 for a UCS-2 character, at most 3 bytes. */
static unsigned utf8(uint32_t ch, char *out)
{
    if (ch < 0x80) {
        out[0] = (char)ch;
        return 1;
    }
    if (ch < 0x800) {
        out[0] = (char)(0xc0 | ch >> 6);
        out[1] = (char)(0x80 | (ch & 0x3f));
        return 2;
    }
    out[0] = (char)(0xe0 | ch >> 12);
    out[1] = (char)(0x80 | (ch >> 6 & 0x3f));
    out[2] = (char)(0x80 | (ch & 0x3f));
    return 3;
}

int64_t fat_next(struct fat_vol *v, const struct fat_node *dir, uint32_t *cursor,
                 struct fat_node *out)
{
    uint16_t lname[260];
    unsigned lfn_seq = 0, lfn_first = 0;
    uint8_t lfn_sum = 0;
    bool lfn_ok = false;
    uint32_t dc = dir->entry_dir == 0 && !dir->first ? v->root : dir->first;

    if (!dir->dir || !valid(v, dc))
        return ERR_BADARG;
    for (uint32_t steps = 0; steps < 1u << 21; steps++) {
        uint32_t pos = (*cursor)++, off;
        uint64_t s;
        uint8_t *p, *d;
        int64_t e = entry_at(v, dc, pos, &s, &off);

        if (e)
            return e;
        if ((e = sector(v, s, &p)))
            return e;
        d = p + off;
        if (d[0] == 0x00) {
            (*cursor)--;            /* the end: stay here */
            return ERR_NOTFOUND;
        }
        if (d[0] == 0xe5) {
            lfn_ok = false;
            continue;
        }
        if ((d[11] & 0x3f) == ATTR_LFN) {
            unsigned seq = d[0] & 0x1f;
            static const uint8_t at[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };

            if (d[0] & 0x40) {      /* the last part comes first */
                if (seq == 0 || seq > 20) {
                    lfn_ok = false;
                    continue;
                }
                lfn_ok = true;
                lfn_seq = seq;
                lfn_sum = d[13];
                lfn_first = pos;
                for (unsigned i = 0; i < 260; i++)
                    lname[i] = 0;
            } else if (!lfn_ok || seq != lfn_seq - 1 || d[13] != lfn_sum) {
                lfn_ok = false;
                continue;
            } else {
                lfn_seq = seq;
            }
            for (unsigned i = 0; i < 13; i++)
                lname[(seq - 1) * 13 + i] = (uint16_t)get_le16(d + at[i]);
            continue;
        }
        if (d[11] & ATTR_VOLUME) {
            lfn_ok = false;
            continue;
        }
        *out = (struct fat_node){
            .first = get_le16(d + 20) << 16 | get_le16(d + 26), .size = get_le32(d + 28),
            .dir = d[11] & ATTR_DIR, .time = (uint16_t)get_le16(d + 22),
            .date = (uint16_t)get_le16(d + 24), .entry_dir = dc, .entry_pos = pos,
        };
        if (lfn_ok && lfn_seq == 1 && checksum(d) == lfn_sum) {
            unsigned n = 0;

            for (unsigned i = 0; i < 260 && lname[i] && lname[i] != 0xffff; i++) {
                if (n + 4 > FAT_NAME)
                    break;
                n += utf8(lname[i], out->name + n);
            }
            out->name[n] = '\0';
            out->lfn = (uint8_t)(pos - lfn_first);
        } else {
            short_name(d, out->name);
        }
        lfn_ok = false;
        if (out->dir)
            out->size = 0;
        if ((out->name[0] == '.' && out->name[1] == '\0') ||
            (out->name[0] == '.' && out->name[1] == '.' && out->name[2] == '\0'))
            continue;
        if (out->first && !valid(v, out->first))
            return ERR_BADARG;
        return 0;
    }
    return ERR_BADARG;              /* a directory that never ends */
}

static bool same_name(const char *a, const char *b, unsigned blen)
{
    unsigned i = 0;

    for (; i < blen; i++)
        if (!a[i] || lower(a[i]) != lower(b[i]))
            return false;
    return a[i] == '\0';
}

static void root_node(const struct fat_vol *v, struct fat_node *out)
{
    *out = (struct fat_node){ .first = v->root, .dir = true };
}

/* Find 'name' (blen bytes) in 'dir'. */
static int64_t find(struct fat_vol *v, const struct fat_node *dir, const char *name, unsigned blen,
                    struct fat_node *out)
{
    struct fat_node d = *dir;       /* 'out' may be 'dir' */
    uint32_t cursor = 0;
    int64_t e;

    while ((e = fat_next(v, &d, &cursor, out)) == 0)
        if (same_name(out->name, name, blen))
            return 0;
    return e;
}

int64_t fat_lookup(struct fat_vol *v, const char *path, struct fat_node *out)
{
    struct fat_node at;

    root_node(v, &at);
    while (*path) {
        unsigned n = 0;

        while (*path == '/')
            path++;
        if (!*path)
            break;
        while (path[n] && path[n] != '/')
            n++;
        if (!at.dir)
            return ERR_NOTFOUND;
        if (n >= FAT_NAME)
            return ERR_BADARG;
        int64_t e = find(v, &at, path, n, &at);
        if (e)
            return e;
        path += n;
    }
    *out = at;
    return 0;
}

/* --- Reading and writing. --- */

int64_t fat_read(struct fat_vol *v, const struct fat_node *f, uint64_t off, void *buf,
                 uint32_t len)
{
    uint32_t cb = cluster_bytes(v), done = 0, c;
    int64_t e;

    if (f->dir)
        return ERR_BADARG;
    if (off >= f->size || len == 0)
        return 0;
    if (len > f->size - off)
        len = (uint32_t)(f->size - off);
    if ((e = nth_cluster(v, f->first, off / cb, &c)))
        return e;
    while (done < len) {
        uint32_t in = (uint32_t)((off + done) % cb), n = cb - in;
        uint64_t s;

        if (!c)
            return ERR_BADARG;      /* the chain is shorter than the size */
        if (n > len - done)
            n = len - done;
        s = first_sector(v, c) + in / v->bps;
        /* Whole sectors straight to the buffer, and the whole clusters
         * after this one that follow it on the disk in the same read; a
         * piece of a sector by the cache. */
        if (in % v->bps == 0 && n >= v->bps) {
            uint32_t k = n / v->bps, next;

            while (in + n == cb && len - done - n >= cb && next_cluster(v, c, &next) == 0 && next == c + 1) {
                c = next;
                n += cb;
                k += v->spc;
            }
            if ((e = v->io.read(v->io.ctx, s, k, (uint8_t *)buf + done)))
                return e;
            n = k * v->bps;
        } else {
            uint8_t *p;
            uint32_t at = in % v->bps;

            if (n > v->bps - at)
                n = v->bps - at;
            if ((e = sector(v, s, &p)))
                return e;
            copy((uint8_t *)buf + done, p + at, n);
        }
        done += n;
        if ((off + done) % cb == 0 && done < len && (e = next_cluster(v, c, &c)))
            return e;
    }
    v->walked = (typeof(v->walked)){ f->first, (uint32_t)((off + done - 1) / cb), c };
    return done;
}

/* Write the node's entry back: its first cluster and size. */
static int64_t update_entry(struct fat_vol *v, const struct fat_node *f)
{
    uint64_t s;
    uint32_t off;
    uint8_t *p;
    int64_t e;

    if (!f->entry_dir)
        return 0;                   /* the root has none */
    if ((e = entry_at(v, f->entry_dir, f->entry_pos, &s, &off)) || (e = sector(v, s, &p)))
        return e;
    put16(p + off + 20, f->first >> 16);
    put16(p + off + 26, f->first);
    put32(p + off + 28, f->dir ? 0 : f->size);
    put16(p + off + 22, f->time);
    put16(p + off + 24, f->date);
    return store(v, s);
}

int64_t fat_write(struct fat_vol *v, struct fat_node *f, uint64_t off, const void *buf,
                  uint32_t len)
{
    uint32_t cb = cluster_bytes(v), done = 0, c, prev = 0;
    int64_t e;

    if (v->read_only)
        return ERR_RIGHTS;
    if (f->dir || off > 0xffffffffULL || len > 0xffffffffULL - off)
        return ERR_BADARG;
    if (len == 0)
        return 0;
    if (!f->first) {
        if ((e = alloc_cluster(v, 0, &f->first)))
            return e;
        if ((e = update_entry(v, f)))
            return e;
    }
    /* To the cluster holding 'off', growing the chain as needed. */
    c = f->first;
    for (uint64_t k = 0; k < off / cb; k++) {
        uint32_t n;

        if (k > v->clusters)
            return ERR_BADARG;
        if ((e = next_cluster(v, c, &n)))
            return e;
        if (!n && (e = alloc_cluster(v, c, &n)))
            return e;
        c = n;
    }
    while (done < len) {
        uint32_t in = (uint32_t)((off + done) % cb), n = cb - in, at = in % v->bps;
        uint64_t s = first_sector(v, c) + in / v->bps;

        if (n > len - done)
            n = len - done;
        if (at == 0 && n >= v->bps) {
            uint32_t k = n / v->bps;

            forget(v, s, k);
            if ((e = v->io.write(v->io.ctx, s, k, (const uint8_t *)buf + done)))
                return e;
            n = k * v->bps;
        } else {
            uint8_t *p;

            if (n > v->bps - at)
                n = v->bps - at;
            if ((e = sector(v, s, &p)))
                return e;
            copy(p + at, (const uint8_t *)buf + done, n);
            if ((e = store(v, s)))
                return e;
        }
        done += n;
        if ((off + done) % cb == 0 && done < len) {
            prev = c;
            if ((e = next_cluster(v, prev, &c)))
                return e;
            if (!c && (e = alloc_cluster(v, prev, &c)))
                return e;
        }
    }
    if (off + len > f->size)
        f->size = (uint32_t)(off + len);
    f->date = DATE_2026;
    f->time = 0;
    if ((e = update_entry(v, f)))
        return e;
    return done;
}

int64_t fat_truncate(struct fat_vol *v, struct fat_node *f, uint32_t size)
{
    uint32_t cb = cluster_bytes(v), keep, c, n;
    int64_t e;

    if (v->read_only)
        return ERR_RIGHTS;
    if (f->dir)
        return ERR_BADARG;
    if (size >= f->size)
        return 0;
    keep = (size + cb - 1) / cb;
    if (keep == 0) {
        if (f->first && (e = free_chain(v, f->first)))
            return e;
        f->first = 0;
    } else {
        if ((e = nth_cluster(v, f->first, keep - 1, &c)) || !c)
            return e ? e : ERR_BADARG;
        if ((e = next_cluster(v, c, &n)) || (e = fat_set(v, c, 0x0fffffff)))
            return e;
        if (n && (e = free_chain(v, n)))
            return e;
    }
    f->size = size;
    return update_entry(v, f);
}

/* --- Making and removing. --- */

/* 'name' as an 8.3 name and its case flags; false if it won't fit. */
static bool to_short(const char *name, unsigned len, uint8_t out[11], uint8_t *nt)
{
    unsigned dot = len, b = 0, x = 0;
    bool bl = false, bu = false, xl = false, xu = false;

    for (unsigned i = 0; i < len; i++)
        if (name[i] == '.')
            dot = i;
    if (dot == 0 || dot > 8 || (dot < len && len - dot - 1 > 3) || len == 0)
        return false;
    fill(out, ' ', 11);
    for (unsigned i = 0; i < len; i++) {
        char c = name[i];

        if (i == dot)
            continue;
        if (c == '.' || c == ' ' || (unsigned char)c < 33 || (unsigned char)c > 126 || c == '"' ||
            c == '*' || c == '+' || c == ',' || c == '/' || c == ':' || c == ';' || c == '<' ||
            c == '=' || c == '>' || c == '?' || c == '[' || c == '\\' || c == ']' || c == '|')
            return false;
        if (i < dot) {
            bl |= c >= 'a' && c <= 'z';
            bu |= c >= 'A' && c <= 'Z';
            out[b++] = (uint8_t)upper(c);
        } else {
            xl |= c >= 'a' && c <= 'z';
            xu |= c >= 'A' && c <= 'Z';
            out[8 + x++] = (uint8_t)upper(c);
        }
    }
    if ((bl && bu) || (xl && xu))
        return false;               /* mixed case needs a long name */
    *nt = (bl ? NT_LOWER_BASE : 0) | (xl ? NT_LOWER_EXT : 0);
    return true;
}

/* A long name's: printable ASCII but what FAT forbids in one, no more
 * than 255, not ending in a dot or a space. Characters past ASCII wait. */
static bool long_ok(const char *name, unsigned len)
{
    if (len == 0 || len > 255 || name[len - 1] == '.' || name[len - 1] == ' ')
        return false;
    for (unsigned i = 0; i < len; i++) {
        char c = name[i];

        if ((unsigned char)c < 32 || (unsigned char)c > 126 || c == '"' || c == '*' || c == '/' || c == ':' ||
            c == '<' || c == '>' || c == '?' || c == '\\' || c == '|')
            return false;
    }
    return true;
}

/* A long name's short one, "BASIS~N.EXT": its first six characters
 * that may be in one, upper case, and its last extension's three. */
static void alias(const char *name, unsigned len, unsigned n, uint8_t out[11])
{
    static const char bad[] = " .\"*+,/:;<=>?[\\]|";
    unsigned dot = len, b = 0, x = 0;
    char tail[4];
    unsigned t = 0;

    for (unsigned i = 0; i < len; i++)
        if (name[i] == '.')
            dot = i;
    fill(out, ' ', 11);
    for (unsigned i = 0; i < dot && b < 6; i++) {
        bool ok = true;

        for (unsigned k = 0; bad[k]; k++)
            ok &= name[i] != bad[k];
        if (ok)
            out[b++] = (uint8_t)upper(name[i]);
    }
    if (b == 0)
        out[b++] = '_';
    for (unsigned i = dot + 1; i < len && x < 3; i++) {
        bool ok = true;

        for (unsigned k = 0; bad[k]; k++)
            ok &= name[i] != bad[k];
        if (ok)
            out[8 + x++] = (uint8_t)upper(name[i]);
    }
    do
        tail[t++] = (char)('0' + n % 10);
    while ((n /= 10) && t < 3);
    if (b + 1 + t > 8)
        b = 8 - 1 - t;
    out[b++] = '~';
    while (t)
        out[b++] = (uint8_t)tail[--t];
}

/* Whether a short name is in the directory starting at cluster dc. */
static int64_t short_taken(struct fat_vol *v, uint32_t dc, const uint8_t name[11], bool *taken)
{
    *taken = false;
    for (uint32_t pos = 0; pos < 1u << 16; pos++) {
        uint64_t s;
        uint32_t off;
        uint8_t *p;
        int64_t e = entry_at(v, dc, pos, &s, &off);

        if (e == ERR_NOTFOUND)
            return 0;
        if (e || (e = sector(v, s, &p)))
            return e;
        if (p[off] == 0x00)
            return 0;
        if (p[off] != 0xe5 && (p[off + 11] & 0x3f) != ATTR_LFN) {
            bool same = true;

            for (unsigned i = 0; i < 11; i++)
                same &= p[off + i] == name[i];
            if (same) {
                *taken = true;
                return 0;
            }
        }
    }
    return 0;
}

/* The parent's node and the last part of 'path'. */
static int64_t split(struct fat_vol *v, const char *path, struct fat_node *parent,
                     const char **leaf, unsigned *len)
{
    char dir[FAT_NAME];
    unsigned n = 0, last = 0;

    while (path[n])
        n++;
    while (n && path[n - 1] == '/')
        n--;
    for (unsigned i = 0; i < n; i++)
        if (path[i] == '/')
            last = i + 1;
    if (n == last || last >= FAT_NAME)
        return ERR_BADARG;
    copy(dir, path, last);
    dir[last] = '\0';
    *leaf = path + last;
    *len = n - last;
    int64_t e = fat_lookup(v, dir, parent);
    if (e)
        return e;
    return parent->dir ? 0 : ERR_NOTFOUND;
}

int64_t fat_create(struct fat_vol *v, const char *path, bool dir, struct fat_node *out)
{
    struct fat_node parent, other;
    const char *leaf;
    unsigned len, lfn = 0, run = 0;
    uint8_t name[11], nt = 0;
    uint32_t pc, pos;
    int64_t e;

    if (v->read_only)
        return ERR_RIGHTS;
    if ((e = split(v, path, &parent, &leaf, &len)))
        return e;
    if ((e = find(v, &parent, leaf, len, &other)) != ERR_NOTFOUND)
        return e ? e : ERR_EXISTS;
    pc = parent.first ? parent.first : v->root;
    /* A name 8.3 can't hold is a long one (V8 step 7), with a short alias. */
    if (!to_short(leaf, len, name, &nt)) {
        bool taken = true;

        if (!long_ok(leaf, len))
            return ERR_BADARG;
        for (unsigned n = 1; taken && n < 1000; n++) {
            alias(leaf, len, n, name);
            if ((e = short_taken(v, pc, name, &taken)))
                return e;
        }
        if (taken)
            return ERR_EXISTS;
        lfn = (len + 12) / 13;
    }

    /* lfn + 1 free entries in a row, or new clusters on the end of the directory. */
    for (pos = 0;; pos++) {
        uint64_t s;
        uint32_t off;
        uint8_t *p;

        if (pos >= 1u << 16)
            return ERR_FULL;        /* FAT's limit on a directory */
        e = entry_at(v, pc, pos, &s, &off);
        if (e == ERR_NOTFOUND) {
            uint32_t last, c;

            if ((e = nth_cluster(v, pc, pos / (cluster_bytes(v) / 32) - 1, &last)) ||
                (e = alloc_cluster(v, last, &c)))
                return e;
            pos--;                  /* its first entry, next time round */
            continue;
        }
        if (e || (e = sector(v, s, &p)))
            return e;
        run = p[off] == 0x00 || p[off] == 0xe5 ? run + 1 : 0;
        if (run == lfn + 1)
            break;
    }
    /* The long name's entries, its last part first, then the short one. */
    for (unsigned k = 0; k < lfn; k++) {
        static const uint8_t at[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
        unsigned part = lfn - k;    /* 1 is the name's start */
        uint64_t s;
        uint32_t off;
        uint8_t *p, *d;

        if ((e = entry_at(v, pc, pos - lfn + k, &s, &off)) || (e = sector(v, s, &p)))
            return e;
        d = p + off;
        fill(d, 0, 32);
        d[0] = (uint8_t)(part | (k == 0 ? 0x40 : 0));
        d[11] = ATTR_LFN;
        d[13] = checksum(name);
        for (unsigned i = 0; i < 13; i++) {
            unsigned c = (part - 1) * 13 + i;
            uint16_t ch = c < len ? (uint8_t)leaf[c] : c == len ? 0 : 0xffff;

            put16(d + at[i], ch);
        }
        if ((e = store(v, s)))
            return e;
    }
    *out = (struct fat_node){ .dir = dir, .date = DATE_2026, .entry_dir = pc, .entry_pos = pos,
                              .lfn = (uint8_t)lfn };
    copy(out->name, leaf, len);
    out->name[len] = '\0';
    if (dir) {
        /* Its first cluster, with "." and "..". */
        uint8_t *p;
        uint64_t s;

        if ((e = alloc_cluster(v, 0, &out->first)))
            return e;
        s = first_sector(v, out->first);
        if ((e = sector(v, s, &p)))
            return e;
        for (unsigned k = 0; k < 2; k++) {
            uint8_t *d = p + 32 * k;
            uint32_t c = k ? (pc == v->root ? 0 : pc) : out->first;

            fill(d, ' ', 11);
            d[0] = '.';
            if (k)
                d[1] = '.';
            d[11] = ATTR_DIR;
            put16(d + 20, c >> 16);
            put16(d + 26, c);
            put16(d + 24, DATE_2026);
        }
        if ((e = store(v, s)))
            return e;
    }
    {
        uint64_t s;
        uint32_t off;
        uint8_t *p, *d;

        if ((e = entry_at(v, pc, pos, &s, &off)) || (e = sector(v, s, &p)))
            return e;
        d = p + off;
        fill(d, 0, 32);
        copy(d, name, 11);
        if (d[0] == 0xe5)
            d[0] = 0x05;
        d[11] = dir ? ATTR_DIR : ATTR_ARCHIVE;
        d[12] = nt;
        put16(d + 16, DATE_2026);
        put16(d + 18, DATE_2026);
        put16(d + 24, DATE_2026);
        put16(d + 20, out->first >> 16);
        put16(d + 26, out->first);
        return store(v, s);
    }
}

int64_t fat_remove(struct fat_vol *v, const char *path)
{
    struct fat_node f, child;
    uint32_t cursor = 0;
    int64_t e;

    if (v->read_only)
        return ERR_RIGHTS;
    if ((e = fat_lookup(v, path, &f)))
        return e;
    if (!f.entry_dir)
        return ERR_BADARG;          /* the root */
    if (f.dir && (e = fat_next(v, &f, &cursor, &child)) != ERR_NOTFOUND)
        return e ? e : ERR_EXISTS;  /* not empty */
    /* The entry and its long-name ones, deleted; then its clusters free. */
    for (uint32_t k = 0; k <= f.lfn; k++) {
        uint64_t s;
        uint32_t off;
        uint8_t *p;

        if ((e = entry_at(v, f.entry_dir, f.entry_pos - k, &s, &off)) || (e = sector(v, s, &p)))
            return e;
        p[off] = 0xe5;
        if ((e = store(v, s)))
            return e;
    }
    return f.first ? free_chain(v, f.first) : 0;
}

uint32_t fat_free(struct fat_vol *v)
{
    uint32_t n = 0, val;

    if (v->free != UINT32_MAX)
        return v->free;
    for (uint32_t c = 2; c < v->clusters + 2; c++) {
        if (fat_get(v, c, &val) != 0)
            return n;               /* not kept: counted again next time */
        n += val == 0;
    }
    return v->free = n;
}
