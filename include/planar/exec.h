/*
 * planar/exec.h -- the kernel calls, as user code sees them
 *
 * Exec's names. These are the raw kernel calls, one stub each
 * (arch/<isa>/user/syscalls.S); exec.library will wrap them. Signals
 * 0-15 are the system's, 16-63 the task's. Handles name kernel objects
 * system-wide.
 */
#ifndef PLANAR_EXEC_H
#define PLANAR_EXEC_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

typedef uint32_t Handle;

/* Exec's break signals, among the system's 16. */
#define SIGBREAKF_CTRL_C    (1ULL << 12)

int64_t KLog(const char *text);
[[noreturn]] void ExitProcess(int64_t code);
uint64_t SetSignal(uint64_t bits, uint64_t mask);
uint64_t Wait(uint64_t mask);
int64_t Signal(Handle task, uint64_t mask);
Handle FindTask(const char *name);          /* nullptr: the calling task */
int64_t CreateTask(void (*entry)(uint64_t arg), uint64_t arg);     /* handle, or error */
[[noreturn]] void ExitTask(int64_t code);
int64_t AllocSignal(int64_t bit);           /* -1: any free one */
void FreeSignal(int64_t bit);

/*
 * Areas (docs §3.3): page-rounded memory, mapped at the same address in
 * every process that maps it. Allocated readable and writable; AREAF_EXEC
 * lets ProtectArea make it executable instead, AREAF_RWX both at once.
 * AREAF_NC is for memory a device reads without snooping the CPU's
 * caches (docs §5.2): every mapping of it is normal memory the CPU
 * doesn't cache, whose writes may be gathered (write-combined); a
 * barrier orders them before a doorbell. ERR_NOSYS where the CPU can't
 * map memory so (RISC-V without Svpbmt).
 */
#define AREAF_CLEAR     (1u << 0)   /* zero-filled (they always are) */
#define AREAF_CONTIG    (1u << 1)   /* physically contiguous (they always are) */
#define AREAF_EXEC      (1u << 2)   /* may be made executable */
#define AREAF_RWX       (1u << 3)   /* may be writable and executable at once */
#define AREAF_LARGE     (1u << 4)   /* 2 MiB-aligned */
#define AREAF_DMA32     (1u << 5)   /* below 4 GiB: for a device with 32-bit DMA */
#define AREAF_NC        (1u << 6)   /* not cached by the CPU; its writes may combine */

#define AREAR_READ      (1u << 0)
#define AREAR_WRITE     (1u << 1)
#define AREAR_EXEC      (1u << 2)

struct AreaInfo {
    void *ai_Address;
    uint64_t ai_Size;
    Handle ai_Area;             /* 0 if the kernel owns it (image, stack) */
    uint32_t ai_Rights;         /* the caller's */
    uint32_t ai_Flags;
    uint32_t ai_Sharers;        /* processes mapping it */
};

int64_t AllocArea(uint64_t size, uint32_t flags, void **address);   /* handle, or error */
int64_t FreeArea(Handle area);
int64_t ShareArea(Handle area, Handle task, uint32_t rights);
int64_t UnshareArea(Handle area, Handle task);
/* The caller's own mapping of an Area another process shared with it,
 * let go (v5-plan step 5): ERR_RIGHTS for one of its own or the
 * kernel's, ERR_NOTFOUND if nothing's mapped there. */
int64_t ReleaseArea(const void *address);
int64_t ProtectArea(Handle area, uint32_t rights);
int64_t QueryArea(const void *address, struct AreaInfo *info);

/*
 * Processes (docs §2.3). CreateProcess starts a built-in program in a new
 * process, with 'arg' as its first argument, and returns a handle the
 * caller's process owns. The calling task gets signal 'sigbit' when the
 * process is dead. PROCF_HELD creates it without starting it, for
 * StartProcess once the caller has granted it what it needs (a lease).
 * A process never outlives its handle: CloseHandle kills it if it's
 * still running, and so does its owner's death. QueryProcess(0, ...)
 * asks about the caller's own process (its state is running).
 */
#define PROCF_HELD      (1u << 0)

/* ProcessInfo.pi_State */
#define PROCS_HELD      1
#define PROCS_RUNNING   2           /* or ending */
#define PROCS_EXITED    3           /* pi_ExitCode says how */
#define PROCS_FAULTED   4           /* pi_Fault says how */
#define PROCS_KILLED    5

/* ProcessInfo.pi_Fault */
#define PFAULT_NONE     0
#define PFAULT_READ     1
#define PFAULT_WRITE    2
#define PFAULT_EXEC     3
#define PFAULT_ILLEGAL  4
#define PFAULT_ALIGN    5
#define PFAULT_BREAK    6
#define PFAULT_OTHER    7

struct ProcessInfo {
    int64_t pi_ExitCode;
    uint32_t pi_State;
    uint32_t pi_Fault;
    uint64_t pi_FaultAddress;       /* the address it touched */
    uint64_t pi_FaultPC;
    uint32_t pi_Handles;            /* the handles it holds */
    uint32_t pi_Pad;
    uint64_t pi_Pages;              /* pages it maps, each Area once */
    uint64_t pi_LibraryPages;       /* of those, the shared library region's */
};

int64_t CreateProcess(const char *name, uint64_t arg, int64_t sigbit, uint32_t flags);

/*
 * A program its parent loaded (V4 step 6: dos.library's ELF loader, §2.2:
 * none in the kernel). pi_Area is an Area the caller allocated with
 * AREAF_EXEC, owns and alone maps, holding the program laid out as it
 * runs: code and constants in the first pi_Etext bytes (a page multiple),
 * then data and .bss. The Area becomes the new process's: the caller
 * stops mapping it, and its handle goes. The code is mapped read and
 * execute, the rest read and write, and the first task starts pi_Entry
 * bytes in. Otherwise as CreateProcess.
 */
struct ProcessImage {
    Handle pi_Area;
    uint32_t pi_Pad;
    uint64_t pi_Etext;
    uint64_t pi_Entry;
    char pi_Name[16];               /* for its log lines */
};

int64_t CreateProcessImage(const struct ProcessImage *image, uint64_t arg, int64_t sigbit,
                           uint32_t flags);
int64_t StartProcess(Handle process);
int64_t KillProcess(Handle process);
int64_t QueryProcess(Handle process, struct ProcessInfo *info);
int64_t CloseHandle(Handle handle);

/*
 * Time. CurrentTime is nanoseconds since boot. ArmTimer is the calling
 * task's one alarm: signal 'sigbit' once CurrentTime reaches
 * 'deadline' (at once if it has), replacing any alarm armed before; a
 * deadline of 0 disarms it.
 */
uint64_t CurrentTime(void);
int64_t ArmTimer(uint64_t deadline, int64_t sigbit);

/*
 * The whole machine's counts since boot, for measuring what an operation
 * costs (docs §11.8: traps and crossings per I/O). Each is summed over
 * the CPUs, so a count read while others run is approximate.
 */
struct SystemInfo {
    uint64_t si_Calls;              /* kernel calls */
    uint64_t si_Interrupts;         /* from the interrupt controller and the timer */
    uint64_t si_Switches;           /* task switches */
    uint64_t si_Crossings;          /* of those, from one process's domain to another */
    uint64_t si_Quarantined;        /* DMA pages waiting for their device to stop */
};

int64_t QuerySystem(struct SystemInfo *info);

/*
 * Waiting on a word in memory (docs §4.1, §5.1): a sequence number
 * that a device, a server or another task advances. WaitAddr sleeps
 * while the 64-bit word at 'address' (8-byte aligned, readable) holds
 * 'value', until WakeAddr is called on it, the deadline (CurrentTime's
 * nanoseconds; 0 for none) passes, or the task is killed. It returns 0
 * at once if the word holds something else, and 0 when woken, which
 * may be early: the caller looks at the word again. ERR_TIMEOUT if the
 * deadline passed. Words are matched by physical address, so the
 * waiter and the waker may be in different processes, each mapping the
 * word where it is (the identity layout). WakeAddr wakes up to 'count'
 * tasks waiting on the word and returns how many it woke.
 *
 * For sequence numbers only, never as a lock between processes (§12.5):
 * a holder that dies would leave the others waiting.
 */
int64_t WaitAddr(const volatile uint64_t *address, uint64_t value, uint64_t deadline);
int64_t WakeAddr(const volatile uint64_t *address, uint32_t count);

/*
 * Memory, by Exec's names (V6 step 1): over the runtime's memmove, memcpy
 * and memset (lib/rt, by the ISA's vector unit), which every program
 * links. Exec's order: the source first. CopyMem copies overlapping
 * ranges correctly, which Exec didn't promise. CopyMemQuick is for
 * 8-byte-aligned source, destination and size, and takes no care over
 * overlap. SetMem fills, as AmigaOS 4's does.
 */
void *memcpy(void *restrict dst, const void *restrict src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);

static inline void CopyMem(const void *source, void *dest, uint64_t size)
{
    memmove(dest, source, size);
}

static inline void CopyMemQuick(const void *source, void *dest, uint64_t size)
{
    memcpy(dest, source, size);
}

static inline void *SetMem(void *dest, uint8_t fill, uint64_t size)
{
    return memset(dest, fill, size);
}

/*
 * Pools (V6 step 2; Exec V39, docs §17.4.2). Blocks of any size from
 * puddles, Areas of 'puddleSize' bytes; one above 'threshSize' is an
 * Area of its own. The flags are the Areas' (AREAF_DMA32, AREAF_NC);
 * with AREAF_CLEAR every block is zeroed, and with POOLF_LOCKED the
 * process's tasks may share the pool. FreePooled takes the size the
 * block was allocated with, which is all it knows of it. DeletePool
 * frees every block at once. CreatePoolMem makes a pool in 'size' bytes
 * at 'buf', which never grows. Blocks are 16-byte aligned.
 */
#define POOLF_LOCKED    (1u << 31)  /* a lock around each call */

struct Pool;

struct Pool *CreatePool(uint32_t flags, uint64_t puddleSize, uint64_t threshSize);
struct Pool *CreatePoolMem(void *buf, uint64_t size);
void DeletePool(struct Pool *pool);
void *AllocPooled(struct Pool *pool, uint64_t memSize);
void FreePooled(struct Pool *pool, void *memory, uint64_t memSize);

/*
 * AllocVec (V36): a block that knows its size, from a pool the process
 * makes at its first call, shared by its tasks. AREAF_CLEAR zeroes it;
 * AREAF_DMA32 or AREAF_NC makes it an Area of its own, so placed or so
 * mapped, with the block 16 bytes in.
 */
void *AllocVec(uint64_t byteSize, uint32_t requirements);
void FreeVec(void *memoryBlock);

/*
 * RawDoFmt (docs §17.4.2): C's printf conversions but floating point's,
 * each byte to putChProc, then a NUL. %d is an int and %ld 64 bits, as
 * C's are (Exec's were 16 and 32); C23's %b and %wN; %S a struct Str
 * (planar/utility.h). Returns the bytes before the NUL.
 */
typedef void (*PutChProc)(char ch, void *putChData);

uint64_t RawDoFmt(const char *formatString, va_list dataStream, PutChProc putChProc,
                  void *putChData);

/*
 * Libraries and interfaces (V6 step 4, docs §7.2, AmigaOS 4's model).
 * A shared library is code and constants the kernel relocates once at
 * boot into the shared library region, mapped read and execute into
 * every process at the same address. Its base says its name and version
 * and lists its interfaces; an interface is a table of functions, called
 * as IExec->AllocVec(...). Libraries keep nothing of their own: what one
 * needs per process is in the caller's memory.
 */
struct Interface;

struct Library {
    const char *lib_Name;           /* "exec.library" */
    uint16_t lib_Version, lib_Revision;
    uint32_t lib_Pad;
    const char *lib_IdString;
    struct Interface *const *lib_Interfaces;    /* ends with nullptr */
};

struct Interface {
    const char *if_Name;            /* "main" */
    uint32_t if_Version;
    uint32_t if_Size;               /* bytes, this header and every function */
    struct Library *if_Library;
};

/*
 * exec.library's "main" interface, version 1: the kernel calls and
 * exec's library functions above, by their names. Its order is the ABI:
 * a new version only adds at the end. CopyMem, CopyMemQuick and SetMem
 * are the vector unit's where the CPU has one, chosen at boot.
 */
struct ExecIFace {
    struct Interface Data;

    struct Library *(*OpenLibrary)(const char *name, uint32_t version);
    void (*CloseLibrary)(struct Library *library);
    struct Interface *(*GetInterface)(struct Library *library, const char *name,
                                      uint32_t version);
    void (*DropInterface)(struct Interface *iface);

    int64_t (*KLog)(const char *text);
    void (*ExitProcess)(int64_t code);
    uint64_t (*SetSignal)(uint64_t bits, uint64_t mask);
    uint64_t (*Wait)(uint64_t mask);
    int64_t (*Signal)(Handle task, uint64_t mask);
    Handle (*FindTask)(const char *name);
    int64_t (*CreateTask)(void (*entry)(uint64_t arg), uint64_t arg);
    void (*ExitTask)(int64_t code);
    int64_t (*AllocSignal)(int64_t bit);
    void (*FreeSignal)(int64_t bit);

    int64_t (*AllocArea)(uint64_t size, uint32_t flags, void **address);
    int64_t (*FreeArea)(Handle area);
    int64_t (*ShareArea)(Handle area, Handle task, uint32_t rights);
    int64_t (*UnshareArea)(Handle area, Handle task);
    int64_t (*ReleaseArea)(const void *address);
    int64_t (*ProtectArea)(Handle area, uint32_t rights);
    int64_t (*QueryArea)(const void *address, struct AreaInfo *info);

    int64_t (*CreateProcess)(const char *name, uint64_t arg, int64_t sigbit, uint32_t flags);
    int64_t (*CreateProcessImage)(const struct ProcessImage *image, uint64_t arg, int64_t sigbit,
                                  uint32_t flags);
    int64_t (*StartProcess)(Handle process);
    int64_t (*KillProcess)(Handle process);
    int64_t (*QueryProcess)(Handle process, struct ProcessInfo *info);
    int64_t (*CloseHandle)(Handle handle);

    uint64_t (*CurrentTime)(void);
    int64_t (*ArmTimer)(uint64_t deadline, int64_t sigbit);
    int64_t (*QuerySystem)(struct SystemInfo *info);
    int64_t (*WaitAddr)(const volatile uint64_t *address, uint64_t value, uint64_t deadline);
    int64_t (*WakeAddr)(const volatile uint64_t *address, uint32_t count);

    void (*CopyMem)(const void *source, void *dest, uint64_t size);
    void (*CopyMemQuick)(const void *source, void *dest, uint64_t size);
    void *(*SetMem)(void *dest, uint8_t fill, uint64_t size);

    struct Pool *(*CreatePool)(uint32_t flags, uint64_t puddleSize, uint64_t threshSize);
    struct Pool *(*CreatePoolMem)(void *buf, uint64_t size);
    void (*DeletePool)(struct Pool *pool);
    void *(*AllocPooled)(struct Pool *pool, uint64_t memSize);
    void (*FreePooled)(struct Pool *pool, void *memory, uint64_t memSize);
    void *(*AllocVec)(uint64_t byteSize, uint32_t requirements);
    void (*FreeVec)(void *memoryBlock);

    uint64_t (*RawDoFmt)(const char *formatString, va_list dataStream, PutChProc putChProc,
                         void *putChData);
};

/* exec.library's main interface, from the kernel at the start (crt0):
 * nullptr where there are no libraries (the hosted build). */
extern struct ExecIFace *IExec;

#endif /* PLANAR_EXEC_H */
