/*
 * kernel/syscall.c -- the kernel call dispatcher
 */
#include <planar/syscalls.h>

#include "arch.h"
#include "area.h"
#include "cpu.h"
#include "futex.h"
#include "handle.h"
#include "irq.h"
#include "kprintf.h"
#include "ktime.h"
#include "lease.h"
#include "pmm.h"
#include "port.h"
#include "process.h"
#include "prof.h"
#include "sched.h"
#include "syscall.h"

constexpr unsigned KLOG_MAX = 200;
constexpr unsigned KLOG_BURST = 32;
constexpr unsigned KLOG_PER_S = 20;

static atomic_uint_fast64_t klog_drops;

/* Signals a user task may not wait for, send or change: the kernel's. */
constexpr uint64_t SIGF_KERNEL = SIGF_SLEEP | SIGF_KILL | SIGF_WAKE | SIGF_PORT | SIGF_LOG;

bool syscall_copy_string(char *dst, uint64_t src, unsigned size)
{
    for (unsigned n = 0; n < size; n++) {
        if (!arch_copy_from_user(&dst[n], src + n, 1))
            return false;
        if (!dst[n])
            return true;
    }
    return false;
}

bool syscall_sigbit(int64_t bit)
{
    return bit >= 16 && bit < 64;
}

/* Append 's' at 'n' in 'd' (room for 'size'), and return the new end. */
static unsigned cat(char *d, unsigned n, unsigned size, const char *s)
{
    while (*s && n < size)
        d[n++] = *s++;
    return n;
}

static unsigned cat_u64(char *d, unsigned n, unsigned size, uint64_t v)
{
    char digits[21];
    unsigned k = sizeof digits;

    digits[--k] = '\0';
    do {
        digits[--k] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    return cat(d, n, size, &digits[k]);
}

/*
 * A line costs the caller a copy into the log's ring, which a kernel
 * task drains to the console (kernel/kprintf.c, review M12). A process
 * gets KLOG_BURST lines at once and KLOG_PER_S a second after that, the
 * rest only counted, and only printable bytes reach the terminal. One
 * that finds the ring full waits for room, as it waited for the polled
 * UART before.
 */
static int64_t sys_klog(struct task *self, uint64_t text)
{
    struct process *p = self->process;
    char buf[KLOG_MAX + 1], line[2 * KLOG_MAX];
    uint64_t now = time_now(), dropped = 0;
    unsigned n;

    for (n = 0; n < KLOG_MAX; n++) {
        if (!arch_copy_from_user(&buf[n], text + n, 1))
            return ERR_BADADDR;
        if (!buf[n])
            break;
        if (buf[n] < 0x20 || buf[n] > 0x7e)
            buf[n] = '?';
    }
    buf[n] = '\0';
    if (p) {
        unsigned long was = spin_lock_irqsave(&p->lock);
        uint64_t refill = (now - p->klog_at) * KLOG_PER_S / (time_hz() ? time_hz() : 1);
        bool ok;

        if (refill) {
            p->klog_tokens = p->klog_tokens + refill > KLOG_BURST ? KLOG_BURST
                                                                  : p->klog_tokens + (unsigned)refill;
            p->klog_at = now;
        }
        if ((ok = p->klog_tokens > 0)) {
            p->klog_tokens--;
            dropped = p->klog_dropped;
            p->klog_dropped = 0;
        } else {
            p->klog_dropped++;
        }
        spin_unlock_irqrestore(&p->lock, was);
        if (!ok) {
            atomic_fetch_add(&klog_drops, 1);
            return 0;
        }
    }
    n = 0;
    if (dropped) {
        n = cat(line, n, sizeof line, "[");
        n = cat(line, n, sizeof line, self->name);
        n = cat(line, n, sizeof line, "] (");
        n = cat_u64(line, n, sizeof line, dropped);
        n = cat(line, n, sizeof line, " lines dropped)\r\n");
    }
    n = cat(line, n, sizeof line, "[");
    n = cat(line, n, sizeof line, self->name);
    n = cat(line, n, sizeof line, "] ");
    n = cat(line, n, sizeof line - 2, buf);
    n = cat(line, n, sizeof line, "\r\n");
    while (!klog_user(line, n)) {
        if (atomic_load(&self->killed))
            return 0;
        task_sleep_until(time_now() + time_us(1000));
    }
    return 0;
}

uint64_t syscall_klog_drops(void)
{
    return atomic_load(&klog_drops);
}

struct signal_args {
    struct process *from;
    uint64_t mask;
    bool ok;
};

/* Only within the caller's own process: across processes there's
 * SignalConn, to a peer (docs/10-code-review.md M10). */
static void signal_fn(void *obj, void *arg)
{
    struct signal_args *s = arg;
    struct task *t = obj;

    if ((s->ok = t->process == s->from))
        task_signal(t, s->mask);
}

static int64_t sys_signal(struct task *self, Handle h, uint64_t mask)
{
    struct signal_args s = { .from = self->process, .mask = mask & ~SIGF_KERNEL };

    return handle_with(h, HANDLE_TASK, signal_fn, &s) && s.ok ? 0 : ERR_BADHANDLE;
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

static void alarm_fire(void *data)
{
    struct task *t = data;

    task_signal(t, t->alarm_sig);
}

static int64_t sys_arm_timer(struct task *self, uint64_t deadline, int64_t sigbit)
{
    if (deadline && !syscall_sigbit(sigbit))
        return ERR_BADARG;          /* and the alarm stays as it was */
    if (deadline && !time_have_timers())
        return ERR_NOSYS;
    ktimer_stop(&self->alarm);
    if (deadline == 0)
        return 0;
    self->alarm = (struct ktimer){ .fn = alarm_fire, .data = self };
    self->alarm_sig = 1ULL << sigbit;
    ktimer_start(&self->alarm, time_from_ns(deadline));
    return 0;
}

/* CloseHandle: for the handles that have no call of their own. Tasks,
 * Areas, ports and connections are let go by ExitTask, FreeArea,
 * DeletePort and CloseConn. */
static int64_t sys_close_handle(struct task *self, Handle h)
{
    struct process *p = handle_take(h, HANDLE_PROCESS, self->process);

    if (!p)
        return lease_close_call(h);
    process_release(p);
    return 0;
}

static int64_t dispatch(uint64_t nr, const uint64_t a[6])
{
    struct task *self = task_self();

    {
        /* Interrupts off: a move between the load and the store would
         * have two CPUs writing one count (§10.8). */
        unsigned long was = arch_irq_save();

        cpu_count(&this_cpu()->syscalls);
        arch_irq_restore(was);
    }
    cpu_count(&self->calls);
    prof_mark(P_SYSCALL);
    switch (nr) {
    case SYS_KLOG:
        return sys_klog(self, a[0]);
    case SYS_EXIT_PROCESS:
        process_exit((int64_t)a[0]);
    case SYS_SET_SIGNAL:
        return (int64_t)task_set_signal(a[0], a[1] & ~SIGF_KERNEL);
    case SYS_WAIT:
        return (int64_t)task_wait(a[0] & ~SIGF_KERNEL);
    case SYS_WAIT_UNTIL:
        return (int64_t)task_wait_until(a[0] & ~SIGF_KERNEL, a[1], nullptr);
    case SYS_SIGNAL:
        return sys_signal(self, (Handle)a[0], a[1]);
    case SYS_FIND_TASK:
        return a[0] ? ERR_NOTFOUND : self->handle;
    case SYS_CREATE_TASK:
        return process_create_task(a[0], a[1], a[2]);
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
    case SYS_RELEASE_AREA:
        return area_release_call(a[0]);
    case SYS_PROTECT_AREA:
        return area_protect_call((uint32_t)a[0], (uint32_t)a[1]);
    case SYS_QUERY_AREA:
        return area_query_call(a[0], a[1]);
    case SYS_CREATE_PORT:
        return port_create_call(a[0], (int64_t)a[1]);
    case SYS_DELETE_PORT:
        return port_delete_call((uint32_t)a[0]);
    case SYS_OPEN_PORT:
        return port_open_call(a[0], (int64_t)a[1], a[2], a[3], a[4], a[5]);
    case SYS_ACCEPT_CONN:
        return port_accept_call((uint32_t)a[0], (int64_t)a[1], a[2]);
    case SYS_SIGNAL_CONN:
        return conn_signal_call((uint32_t)a[0]);
    case SYS_SIGNAL_WAIT_CONN:
        return (int64_t)conn_signal_wait_call((uint32_t)a[0], a[1] & ~SIGF_KERNEL, a[2]);
    case SYS_CLOSE_CONN:
        return conn_close_call((uint32_t)a[0]);
    case SYS_CREATE_PROCESS:
        return process_create_call(a[0], a[1], (int64_t)a[2], (uint32_t)a[3]);
    case SYS_CREATE_PROCESS_IMAGE:
        return process_create_image_call(a[0], a[1], (int64_t)a[2], (uint32_t)a[3]);
    case SYS_START_PROCESS:
        return process_start_call((uint32_t)a[0]);
    case SYS_KILL_PROCESS:
        return process_kill_call((uint32_t)a[0]);
    case SYS_QUERY_PROCESS:
        return process_query_call((uint32_t)a[0], a[1]);
    case SYS_QUERY_TASKS:
        return process_tasks_call((uint32_t)a[0], a[1], (uint32_t)a[2]);
    case SYS_CLOSE_HANDLE:
        return sys_close_handle(self, (Handle)a[0]);
    case SYS_CURRENT_TIME:
        return (int64_t)time_ns(time_now());
    case SYS_ARM_TIMER:
        return sys_arm_timer(self, a[0], (int64_t)a[1]);
    case SYS_CREATE_LEASE:
        return lease_create_call((uint32_t)a[0], a[1], a[2], a[3], (uint32_t)a[4]);
    case SYS_GRANT_LEASE:
        return lease_grant_call((uint32_t)a[0], (uint32_t)a[1]);
    case SYS_REVOKE_LEASE:
        return lease_revoke_call((uint32_t)a[0]);
    case SYS_QUERY_LEASE:
        return lease_query_call((uint32_t)a[0], a[1]);
    case SYS_MAP_LEASE:
        return lease_map_call((uint32_t)a[0], a[1]);
    case SYS_BIND_IRQ:
        return lease_bind_call((uint32_t)a[0], (uint32_t)a[1], (int64_t)a[2]);
    case SYS_ACK_WAIT_IRQ:
        return (int64_t)lease_ack_wait_call((uint32_t)a[0], (uint32_t)a[1], a[2] & ~SIGF_KERNEL);
    case SYS_ALLOC_DMA_AREA:
        return lease_dma_call((uint32_t)a[0], a[1], (uint32_t)a[2], a[3]);
    case SYS_MAP_FIRMWARE:
        return lease_firmware_call((uint32_t)a[0], a[1]);
    case SYS_GRANT_FIRMWARE:
        return lease_firmware_grant_call((uint32_t)a[0], (uint32_t)a[1]);
    case SYS_QUERY_SYSTEM: {
        struct SystemInfo si = {
            .si_Calls = syscall_count(), .si_Interrupts = irq_count(),
            .si_Switches = sched_switches(), .si_Crossings = sched_crossings(),
            .si_Quarantined = lease_quarantined(),
            .si_IdleNanos = time_to_us(sched_idle_ticks()) * 1000, .si_CPUs = cpus_online(),
            .si_Pages = pmm_total(), .si_FreePages = pmm_free_pages(),
        };

        return arch_copy_to_user(a[0], &si, sizeof si) ? 0 : ERR_BADADDR;
    }
    case SYS_GRANT_CONSOLE:
        return lease_console_grant_call((uint32_t)a[0], (uint32_t)a[1]);
    case SYS_READ_CONSOLE:
        return lease_console_read_call(a[0], (uint32_t)a[1]);
    case SYS_WAIT_ADDR:
        return futex_wait_call(a[0], a[1], a[2]);
    case SYS_WAKE_ADDR:
        return futex_wake_call(a[0], (uint32_t)a[1]);
    case SYS_SHARE_LEASE:
        return lease_share_call((uint32_t)a[0], a[1], a[2], (uint32_t)a[3]);
    case SYS_UNSHARE_LEASE:
        return lease_unshare_call((uint32_t)a[0], a[1], (uint32_t)a[2]);
    case SYS_LIMIT_LEASE_DMA:
        return lease_limit_dma_call((uint32_t)a[0], a[1]);
    case SYS_SET_LEASE_CALL:
        return lease_set_call_call((uint32_t)a[0], (uint32_t)a[1]);
    case SYS_CALL_LEASE:
        return lease_call_call((uint32_t)a[0], a[1]);
    case SYS_ALLOC_MSI:
        return lease_msi_call((uint32_t)a[0], (uint32_t)a[1], (uint32_t)a[2], a[3]);
    case SYS_GRANT_ROOT:
        return lease_root_grant_call((uint32_t)a[0], (uint32_t)a[1]);
    case SYS_COLD_REBOOT:
        return lease_cold_reboot_call((uint32_t)a[0]);
    case SYS_READ_LOG:
        return lease_read_log_call((uint32_t)a[0], a[1], (uint32_t)a[2]);
    case SYS_KILL_PEER:
        return lease_kill_peer_call((uint32_t)a[0], (uint32_t)a[1], (uint32_t)a[2]);
    default:
        return ERR_NOSYS;
    }
}

int64_t syscall_dispatch(uint64_t nr, const uint64_t a[6], uint64_t pc, uint64_t lr)
{
    struct task *self = task_self();
    int64_t r;

    /* What it's doing, for its parent's QueryTasks: a few stores, so a
     * stalled server can be seen without a task watching it. */
    self->call = (uint32_t)nr;
    self->call_pc = pc;
    self->call_lr = lr;
    __atomic_store_n(&self->in_call, true, __ATOMIC_RELEASE);
    r = dispatch(nr, a);
    __atomic_store_n(&self->in_call, false, __ATOMIC_RELEASE);

    prof_mark(P_SYSCALL_OUT);
    return r;
}

uint64_t syscall_count(void)
{
    uint64_t n = 0;

    for (unsigned i = 0; i < ncpus; i++)
        n += atomic_load_explicit(&cpus[i].syscalls, memory_order_relaxed);
    return n;
}
