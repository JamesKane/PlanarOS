/*
 * kernel/syscall.c -- the kernel call dispatcher
 */
#include <planar/syscalls.h>

#include "arch.h"
#include "area.h"
#include "handle.h"
#include "kprintf.h"
#include "process.h"
#include "sched.h"
#include "syscall.h"

constexpr unsigned KLOG_MAX = 200;

/* Signals a user task may not wait for, send or change: the kernel's. */
constexpr uint64_t SIGF_KERNEL = SIGF_SLEEP | SIGF_KILL;

static atomic_uint_fast64_t calls;

static int64_t sys_klog(struct task *self, uint64_t text)
{
    char buf[KLOG_MAX + 1];
    unsigned n;

    for (n = 0; n < KLOG_MAX; n++) {
        if (!arch_copy_from_user(&buf[n], text + n, 1))
            return ERR_BADADDR;
        if (!buf[n])
            break;
    }
    buf[n] = '\0';
    kprintf("[%s] %s\n", self->name, buf);
    return 0;
}

static void signal_fn(void *obj, void *arg)
{
    task_signal(obj, *(uint64_t *)arg);
}

static int64_t sys_signal(Handle h, uint64_t mask)
{
    mask &= ~SIGF_KERNEL;
    return handle_with(h, HANDLE_TASK, signal_fn, &mask) ? 0 : ERR_BADHANDLE;
}

/* Exec's AllocSignal: a specific bit, or any free one with -1. */
static int64_t sys_alloc_signal(struct task *t, int64_t bit)
{
    if (bit < 0) {
        for (bit = 16; bit < 64; bit++)
            if (!(t->sig_alloc & (1ULL << bit)))
                break;
    }
    if (bit < 16 || bit >= 64 || (t->sig_alloc & (1ULL << bit)))
        return -1;
    t->sig_alloc |= 1ULL << bit;
    task_set_signal(0, 1ULL << bit);
    return bit;
}

int64_t syscall_dispatch(uint64_t nr, const uint64_t a[6])
{
    struct task *self = task_self();

    atomic_fetch_add(&calls, 1);
    switch (nr) {
    case SYS_KLOG:
        return sys_klog(self, a[0]);
    case SYS_EXIT_PROCESS:
        process_exit((int64_t)a[0]);
    case SYS_SET_SIGNAL:
        return (int64_t)task_set_signal(a[0], a[1] & ~SIGF_KERNEL);
    case SYS_WAIT:
        return (int64_t)task_wait(a[0] & ~SIGF_KERNEL);
    case SYS_SIGNAL:
        return sys_signal((Handle)a[0], a[1]);
    case SYS_FIND_TASK:
        return a[0] ? ERR_NOTFOUND : self->handle;
    case SYS_CREATE_TASK:
        return process_create_task(a[0], a[1]);
    case SYS_EXIT_TASK:
        process_exit_task();
    case SYS_ALLOC_SIGNAL:
        return sys_alloc_signal(self, (int64_t)a[0]);
    case SYS_FREE_SIGNAL:
        if ((int64_t)a[0] >= 16 && (int64_t)a[0] < 64)
            self->sig_alloc &= ~(1ULL << a[0]);
        return 0;
    case SYS_ALLOC_AREA:
        return area_alloc_call(a[0], (uint32_t)a[1], a[2]);
    case SYS_FREE_AREA:
        return area_free_call((uint32_t)a[0]);
    case SYS_SHARE_AREA:
        return area_share_call((uint32_t)a[0], (uint32_t)a[1], (uint32_t)a[2]);
    case SYS_UNSHARE_AREA:
        return area_unshare_call((uint32_t)a[0], (uint32_t)a[1]);
    case SYS_PROTECT_AREA:
        return area_protect_call((uint32_t)a[0], (uint32_t)a[1]);
    case SYS_QUERY_AREA:
        return area_query_call(a[0], a[1]);
    default:
        return ERR_NOSYS;
    }
}

uint64_t syscall_count(void)
{
    return atomic_load(&calls);
}
