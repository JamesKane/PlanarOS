/*
 * planar/dos.h -- the dos server's protocol (servers/dos), and dos.library
 * (lib/dos), which speaks it (docs §2.4, v4-plan step 6)
 *
 * The dos server mounts the FAT32 volumes on every block device it finds
 * and serves files by path, as "VOLUME:dir/file". A volume is named by
 * its label, or, unlabelled, by its device and partition ("nvme0p1").
 * Two names are assigned: SYS:, the volume holding EFI/planar (where
 * planar.efi is), and C:, SYS:EFI/planar/C, where commands are.
 *
 * One message per call, answered at once (a server copies, so the data
 * is in the connection's heap: dm_Buffer). Files are opened by path and
 * named by the number OPEN gives; a client's files close when it goes.
 */
#ifndef PLANAR_DOS_H
#define PLANAR_DOS_H

#include <stdint.h>

#include <planar/msg.h>

constexpr uint32_t DOS_VERSION = 1;
constexpr unsigned DOS_PATH = 256;
constexpr uint32_t DOS_MAX_IO = 65536;      /* bytes a READ or WRITE moves at most */

enum : uint32_t {
    DOS_OPEN = 1,           /* dm_Path, dm_Flags: dm_File, dm_Entry out */
    DOS_CLOSE,              /* dm_File */
    DOS_READ,               /* dm_File, dm_Offset, dm_Length, dm_Buffer: dm_Length out */
    DOS_WRITE,              /* the same, from dm_Buffer */
    DOS_STAT,               /* dm_Path: dm_Entry out */
    DOS_NEXT,               /* dm_File (a directory), dm_Cursor: dm_Entry, dm_Cursor out */
    DOS_REMOVE,             /* dm_Path */
    DOS_MKDIR,              /* dm_Path */
    DOS_VOLUME,             /* dm_Cursor: the nth volume, dm_Entry (its name, size, writable) */
    DOS_TRUNCATE,           /* dm_File, dm_Offset: the new size */
};

/* DOS_OPEN's dm_Flags */
constexpr uint32_t DOSF_WRITE = 1u << 0;    /* open for writing */
constexpr uint32_t DOSF_CREATE = 1u << 1;   /* make it if it isn't there */

struct DosEntry {
    char de_Name[DOS_PATH];
    uint64_t de_Size;               /* a volume's: free bytes */
    uint32_t de_Dir;                /* a volume's: writable */
    uint16_t de_Date, de_Time;      /* FAT's encoding */
};

struct DosMsg {
    struct Message dm_Msg;          /* mn_Error: 0, or why */
    uint32_t dm_Version;
    uint32_t dm_Op;
    uint32_t dm_File;
    uint32_t dm_Cursor;
    uint32_t dm_Flags;
    uint32_t dm_Length;
    uint64_t dm_Offset;
    uint64_t dm_Buffer;             /* in the connection's heap */
    char dm_Path[DOS_PATH];
    struct DosEntry dm_Entry;
};

/* --- dos.library --- */

struct dos {
    struct MsgPort mp;
    struct DosMsg *m;
    uint8_t *buf;                   /* DOS_MAX_IO bytes of heap */
    bool open;
};

int64_t dos_open(struct dos *d, int64_t sigbit);        /* connect to "dos" */
void dos_close(struct dos *d);

/* Files: a number, or an error. */
int64_t dos_file(struct dos *d, const char *path, uint32_t flags, struct DosEntry *entry);
int64_t dos_close_file(struct dos *d, uint32_t file);
int64_t dos_read(struct dos *d, uint32_t file, uint64_t off, void *buf, uint32_t len);
int64_t dos_write(struct dos *d, uint32_t file, uint64_t off, const void *buf, uint32_t len);
int64_t dos_stat(struct dos *d, const char *path, struct DosEntry *entry);
int64_t dos_next(struct dos *d, uint32_t dir, uint32_t *cursor, struct DosEntry *entry);
int64_t dos_remove(struct dos *d, const char *path);
int64_t dos_mkdir(struct dos *d, const char *path);
int64_t dos_volume(struct dos *d, uint32_t index, struct DosEntry *entry);

/*
 * The loader (§2.2: none in the kernel). A program on a volume is an ELF
 * static PIE for this machine, linked at 0 as the built-in ones are
 * (arch/<arch>/user/user.ld): code, then page aligned, data and .bss. It
 * relocates itself. dos_run loads it into a new Area and starts it with
 * CreateProcessImage: a process handle, or an error (ERR_BADARG: not a
 * program this machine runs).
 *
 * Its command line, 'args' (up to DOS_ARGS bytes), goes in a page on the
 * end of that Area, and its argument is the line's address: the Area
 * keeps its address in the child (VQ3), so the child reads it there.
 */
constexpr unsigned DOS_ARGS = 1024;

/*
 * A command's line from the shell is its current directory, a newline,
 * then its arguments. dos_args splits what dos_run passed (0 is "") into
 * the two; dos_path makes 'name' a whole path from 'cwd' unless it names
 * its volume ("SYS:x" is as it is; "x" in "SYS:EFI" is "SYS:EFI/x").
 */
void dos_args(uint64_t arg, char *cwd, unsigned cwd_size, const char **args);
void dos_path(const char *cwd, const char *name, char *out, unsigned size);
int64_t dos_run(struct dos *d, const char *path, const char *args, int64_t sigbit, uint32_t flags);

/*
 * The console (a program's Output(), the Amiga's CON:): text to display's
 * "con.0", which draws it and logs it, and so to the serial console. Where
 * there's no framebuffer, a line at a time to the kernel's log. If the
 * console goes (display restarting) it's reconnected once; after that,
 * the log.
 */
struct con {
    struct MsgPort mp;
    void *m;                        /* a DrvRequest */
    bool open;
    bool retried;                   /* reconnected once after the console went */
    int64_t sigbit;
    char line[160];                 /* the log's, without a console */
    unsigned nline;
};

void con_open(struct con *c, int64_t sigbit);
void con_close(struct con *c);
void con_write(struct con *c, const char *text, uint32_t len);
void con_puts(struct con *c, const char *text);
void con_dec(struct con *c, uint64_t v);

#endif /* PLANAR_DOS_H */
