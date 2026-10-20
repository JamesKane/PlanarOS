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
 */
#define AREAF_CLEAR     (1u << 0)   /* zero-filled (they always are) */
#define AREAF_CONTIG    (1u << 1)   /* physically contiguous */
#define AREAF_EXEC      (1u << 2)   /* may be made executable */
#define AREAF_RWX       (1u << 3)   /* may be writable and executable at once */
#define AREAF_LARGE     (1u << 4)   /* 2 MiB-aligned */

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
int64_t ProtectArea(Handle area, uint32_t rights);
int64_t QueryArea(const void *address, struct AreaInfo *info);

#endif /* PLANAR_EXEC_H */
