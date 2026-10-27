/*
 * servers/dos/fat.h -- FAT32 volumes (Microsoft's FAT specification 1.03,
 * and its long names), for the dos server and, for testing, the host
 *
 * Pure C with no library, over two calls that read and write sectors:
 * the dos server's go to a block device (lib/blk), the host's to an
 * image file (servers/dos/host.c). Every value read from the volume is
 * checked before use (a cluster number, a directory entry, a chain that
 * loops), and a volume that fails a check gives errors, never a fault.
 *
 * Names: long names are read; a file or directory made here gets an
 * 8.3 name, lower case kept by the flags Windows uses, so a name that
 * won't fit 8.3 can't be made yet. Lookups ignore case. Paths are
 * "dir/dir/name", from the root.
 *
 * Writes go to the disk as they're made (no cache holds them back), the
 * FAT and its copies first, then the data, then the directory entry, so
 * a crash between leaves lost clusters at worst, not a damaged file.
 */
#ifndef DOS_FAT_H
#define DOS_FAT_H

#include <stdint.h>

constexpr unsigned FAT_NAME = 256;          /* a name's bytes, with its end */
constexpr unsigned FAT_CACHE = 32;          /* sectors kept */

struct fat_io {
    void *ctx;
    /* 'n' sectors from 'sector', of the volume; 0 or an error. */
    int64_t (*read)(void *ctx, uint64_t sector, uint32_t n, void *buf);
    int64_t (*write)(void *ctx, uint64_t sector, uint32_t n, const void *buf);
};

struct fat_vol {
    struct fat_io io;
    uint64_t sectors;               /* the volume's */
    uint32_t bps;                   /* bytes per sector: 512 to 4096 */
    uint32_t spc;                   /* sectors per cluster */
    uint32_t fat_start, fat_size;   /* sectors */
    uint32_t nfats;
    uint32_t data_start;            /* sector of cluster 2 */
    uint32_t clusters;              /* data clusters: 2 to clusters + 1 */
    uint32_t root;                  /* the root directory's first cluster */
    uint32_t next_free;             /* where to look first for a free cluster */
    uint32_t free;                  /* free clusters; UINT32_MAX until counted */
    /* The last cluster a chain was followed to, where the next walk of
     * it starts if it's further on: so reading a file or a directory in
     * order doesn't walk its chain from the start each time. */
    struct { uint32_t first, k, c; } walked;
    uint32_t fsinfo;                /* its sector; 0 if none */
    bool fsinfo_stale;              /* its free count marked unknown */
    char label[12];
    bool read_only;
    /* The sector cache: FAT and directory sectors. */
    struct { uint64_t sector; uint32_t used; bool valid; } cache[FAT_CACHE];
    uint8_t *cache_mem;             /* FAT_CACHE sectors, the caller's */
    uint8_t *scratch;               /* one more: a partial sector being written */
    uint32_t tick;
};

/* A file or directory, and where its entry is (to update it). */
struct fat_node {
    char name[FAT_NAME];
    uint32_t first;                 /* its first cluster; 0 when empty */
    uint32_t size;                  /* bytes; 0 for a directory */
    bool dir;
    uint16_t date, time;            /* modified, FAT's encoding */
    uint32_t entry_dir;             /* the directory holding its entry: its first cluster; 0 for the root */
    uint32_t entry_pos;             /* the entry's index there */
    uint8_t lfn;                    /* long-name entries just before it */
};

/* Mount the volume: check its boot sector says FAT32 (a FAT12 or FAT16
 * volume is refused) and that its geometry fits in 'sectors'. 'cache'
 * is FAT_CACHE + 1 sectors, the caller's (FAT_MEM bytes is enough for
 * any sector size). 0, or an error (ERR_NOTFOUND: not FAT32). */
constexpr uint32_t FAT_MEM = (FAT_CACHE + 1) * 4096;
int64_t fat_mount(struct fat_vol *v, const struct fat_io *io, uint64_t sectors, void *cache);

/* The node at 'path' ("" or "/" is the root). */
int64_t fat_lookup(struct fat_vol *v, const char *path, struct fat_node *out);

/* A directory's entries in turn: *cursor starts at 0; ERR_NOTFOUND
 * after the last. Volume labels, deleted entries, "." and ".." are
 * skipped. */
int64_t fat_next(struct fat_vol *v, const struct fat_node *dir, uint32_t *cursor,
                 struct fat_node *out);

/* Bytes read (short at the end of the file), or an error. */
int64_t fat_read(struct fat_vol *v, const struct fat_node *f, uint64_t off, void *buf,
                 uint32_t len);

/* Bytes written, growing the file as needed, or an error (ERR_FULL: no
 * clusters left). The node's size and first cluster are updated, and so
 * is its directory entry. */
int64_t fat_write(struct fat_vol *v, struct fat_node *f, uint64_t off, const void *buf,
                  uint32_t len);

/* Make an empty file or directory at 'path', whose parent must exist
 * and which mustn't. */
int64_t fat_create(struct fat_vol *v, const char *path, bool dir, struct fat_node *out);

/* Remove the file, or the empty directory, at 'path'. */
int64_t fat_remove(struct fat_vol *v, const char *path);

/* Cut the file to 'size' bytes or fewer. */
int64_t fat_truncate(struct fat_vol *v, struct fat_node *f, uint32_t size);

/* Free clusters, counted (slow: the whole FAT). */
uint32_t fat_free(struct fat_vol *v);

#endif /* DOS_FAT_H */
