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

#include <stdarg.h>
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

/* --- dos.library (V6 step 6: the RKM's names, docs §17.4.4) --- */

struct Dos {
    struct MsgPort ds_Port;
    struct DosMsg *ds_Msg;
    uint8_t *ds_Buffer;             /* DOS_MAX_IO bytes of heap */
    bool ds_Open;
};

/* Every call to the server has a deadline (V6 step 7): ERR_TIMEOUT past
 * it, when hwdb is told dos is unresponsive, and ERR_BUSY after until
 * dos answers the one it missed. */
constexpr uint64_t DOS_DEADLINE_MS = 10000;

int64_t OpenDos(struct Dos *dos, int64_t sigBit);       /* connect to "dos" */
/* The same, waiting for dos until 'deadline' (ERR_TIMEOUT): at boot. */
int64_t OpenDosUntil(struct Dos *dos, int64_t sigBit, uint64_t deadline);
void CloseDos(struct Dos *dos);

/* Files: a number, or an error. */
int64_t OpenFile(struct Dos *dos, const char *path, uint32_t flags, struct DosEntry *entry);
int64_t CloseFile(struct Dos *dos, uint32_t file);
int64_t ReadFile(struct Dos *dos, uint32_t file, uint64_t offset, void *buffer, uint32_t length);
int64_t WriteFile(struct Dos *dos, uint32_t file, uint64_t offset, const void *buffer,
                  uint32_t length);
int64_t ExaminePath(struct Dos *dos, const char *path, struct DosEntry *entry);
int64_t ExamineNext(struct Dos *dos, uint32_t dir, uint32_t *cursor, struct DosEntry *entry);
int64_t DeletePath(struct Dos *dos, const char *path);
int64_t CreateDir(struct Dos *dos, const char *path);
int64_t NextVolume(struct Dos *dos, uint32_t index, struct DosEntry *entry);

/*
 * The loader (§2.2: none in the kernel). A program on a volume is an ELF
 * static PIE for this machine, linked at 0 as the built-in ones are
 * (arch/<arch>/user/user.ld): code, then page aligned, data and .bss. It
 * relocates itself. RunCommand loads it into a new Area and starts it
 * with CreateProcessImage: a process handle, or an error (ERR_BADARG: not
 * a program this machine runs).
 *
 * Its command line, 'args' (up to DOS_ARGS bytes), goes in a page on the
 * end of that Area, and its argument is the line's address: the Area
 * keeps its address in the child (VQ3), so the child reads it there.
 */
constexpr unsigned DOS_ARGS = 1024;

int64_t RunCommand(struct Dos *dos, const char *path, const char *args, int64_t sigBit,
                   uint32_t flags);

/*
 * A command's line from the Shell is its current directory, a newline,
 * then its arguments. SplitArgs splits what RunCommand passed (0 is "")
 * into the two.
 */
void SplitArgs(uint64_t arg, char *cwd, unsigned cwdSize, const char **args);

/*
 * Paths (V36's). AddPart puts 'fileName' on the end of the path in
 * 'dirName', in place: a name with a volume ("SYS:x") replaces it, one
 * starting ':' goes from its volume, and each leading '/' is a step up.
 * False, with 'dirName' as it was, if the result wouldn't fit in 'size'.
 * FilePart is where a path's last part starts ("SYS:a/b" is "b");
 * PathPart is where its directory ends ("/b", or "b" after a ':'), so a
 * NUL written there leaves the directory.
 */
bool AddPart(char *dirName, const char *fileName, unsigned size);
char *FilePart(const char *path);
char *PathPart(const char *path);

/*
 * ReadArgs (V36's): a command's arguments by template, as
 * "FROM/A/M,TO/A,ALL/S,LINES/K/N". Items are separated by commas; each is
 * its names (aliases joined by '=': "TO=AS") and modifiers:
 *
 *   /A  required          /K  only after its keyword
 *   /S  a switch: 1        /N  a number: a pointer to an int64_t
 *   /M  many: a nullptr-ended array of pointers, the rest of the
 *       positional words, less those later /A items need
 *   /F  the rest of the line, as it is
 *
 * array[i] is item i's: a string, a number's pointer, a switch's 1, or a
 * /M's array. An item not given is left as it was, so the caller sets
 * defaults first. Words are separated by spaces; "quoted" ones may hold
 * spaces, with *" a quote, *n a newline, *e an escape and ** a star, and
 * are never keywords. A value goes after its keyword, or with '='
 * ("TO=x"). The line is rda_Source; what the strings point into is
 * AllocVec's, until FreeArgs.
 *
 * 0, or ERR_BADARG with rda_Why saying why ("" for a line that's only
 * "?": the caller shows the template), or ERR_NOMEM.
 */
struct RDArgs {
    const char *rda_Source;         /* the line: SplitArgs's 'args' */
    void *rda_Buffer;               /* AllocVec's, FreeArgs's */
    char rda_Why[80];
};

int64_t ReadArgs(const char *argTemplate, int64_t *array, struct RDArgs *rdArgs);
void FreeArgs(struct RDArgs *rdArgs);

/*
 * The console (a program's Output(), the Amiga's CON:): text to display's
 * "con.0", which draws it and logs it, and so to the serial console. Where
 * there's no framebuffer, a line at a time to the kernel's log. If the
 * console goes (display restarting) it's reconnected once; after that,
 * the log. Printf formats as RawDoFmt does, 64 bytes a message. A
 * message the display hasn't answered in CON_DEADLINE_MS is reported to
 * hwdb, and the text goes on as if the console had gone.
 */
constexpr uint64_t CON_DEADLINE_MS = 1000;

struct Con {
    struct MsgPort cn_Port;
    void *cn_Msg;                   /* a DrvRequest */
    bool cn_Open;
    bool cn_Retried;                /* reconnected once after the console went */
    int64_t cn_SigBit;
    char cn_Line[160];              /* the log's, without a console */
    unsigned cn_Count;
};

void OpenCon(struct Con *con, int64_t sigBit);
void CloseCon(struct Con *con);
void WriteCon(struct Con *con, const char *text, uint32_t length);
void PutStr(struct Con *con, const char *string);
uint64_t VPrintf(struct Con *con, const char *format, va_list args);
uint64_t Printf(struct Con *con, const char *format, ...);

#endif /* PLANAR_DOS_H */
