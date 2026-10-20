/*
 * kernel/process.c -- processes: protection domains that own resources
 */
#include <planar/endian.h>
#include <planar/exec.h>
#include <planar/syscalls.h>

#include "cpu.h"
#include "handle.h"
#include "kprintf.h"
#include "ktime.h"
#include "area.h"
#include "kstring.h"
#include "pmm.h"
#include "port.h"
#include "process.h"
#include "sched.h"
#include "syscall.h"
#include "userimg.h"

constexpr uint32_t IMAGE_MAGIC = 0x524e4c50;    /* "PLNR" */
constexpr unsigned STACK_PAGES = 4;             /* 16 KiB user stacks */
constexpr unsigned NAME_MAX = 32;

static_assert(FAULT_OTHER == PFAULT_OTHER && FAULT_WRITE == PFAULT_WRITE, "fault codes are the ABI's");

static atomic_uint live_records;

static const char *const fault_names[] = {
    [FAULT_NONE] = "no fault", [FAULT_READ] = "access fault reading",
    [FAULT_WRITE] = "access fault writing", [FAULT_EXEC] = "access fault executing",
    [FAULT_ILLEGAL] = "illegal instruction at", [FAULT_ALIGN] = "misaligned access to",
    [FAULT_BREAK] = "breakpoint at", [FAULT_OTHER] = "fault at",
};

static const struct user_image *find_image(const char *name)
{
    for (const struct user_image *img = user_images; img->name; img++)
        if (kstreq(img->name, name))
            return img;
    return nullptr;
}

/* A stack Area for a new task in 'p', mapped; its top, or 0. */
static uint64_t new_stack(struct process *p, struct area **out)
{
    struct area *a = area_new(STACK_PAGES, 0, true);

    if (!a)
        return 0;
    if (!area_map(a, p, AREAR_READ | AREAR_WRITE, 0)) {
        area_destroy(a);
        return 0;
    }
    *out = a;
    return area_base(a) + area_size(a);
}

/* --- Tasks of a process. --- */

static void user_start(void *)
{
    struct task *self = task_self();

    user_return();              /* killed before it ever ran */
    arch_user_enter(self->user_pc, self->user_sp, self->user_arg[0], self->user_arg[1]);
}

/* A task in 'p' starting at 'pc' with 'sp', not yet launched. */
static struct task *new_user_task(struct process *p, uint64_t pc, struct area *stack,
                                  uint64_t arg0, uint64_t arg1)
{
    struct task *t = task_new(p->name, p->pri, p->affinity, user_start, nullptr);

    if (!t)
        return nullptr;
    t->user_stack = stack;
    t->process = p;
    t->domain = &p->domain;
    t->user_pc = pc;
    t->user_sp = area_base(stack) + area_size(stack);
    t->user_arg[0] = arg0;
    t->user_arg[1] = arg1;
    t->handle = handle_new(HANDLE_TASK, t, p);
    if (!t->handle) {
        task_discard(t);
        return nullptr;
    }
    return t;
}

/* Add a new task to 'p' and start it: 0, or ERR_NOTFOUND if 'p' is
 * ending, or ERR_LIMIT if it has its fill of tasks (counted here, under
 * the lock, so racing CreateTasks can't pass the limit). */
static int64_t launch(struct process *p, struct task *t)
{
    unsigned long was = spin_lock_irqsave(&p->lock);
    int64_t err = atomic_load(&p->dying)          ? ERR_NOTFOUND
                  : p->ntasks >= PROCESS_TASKS    ? ERR_LIMIT
                                                  : 0;

    if (err) {
        spin_unlock_irqrestore(&p->lock, was);
        handle_free(t->handle);
        area_release(t->user_stack, p);
        task_discard(t);
        return err;
    }
    t->next_in_proc = p->tasks;
    p->tasks = t;
    p->ntasks++;
    atomic_fetch_add(&p->unfinished, 1);
    spin_unlock_irqrestore(&p->lock, was);
    task_launch(t);
    return 0;
}

static void reclaim(struct process *p);
static void signal_task_handle(Handle h, uint64_t sig);

/* Load built-in program 'name' into a new process, watched by 'watcher'
 * or the task 'watch_task' names. With 'held', its first task waits in
 * p->held for StartProcess. */
static struct process *create(const char *name, int pri, uint64_t arg, struct task *watcher,
                              Handle watch_task, uint64_t sig, uint64_t affinity, bool held)
{
    const struct user_image *img = find_image(name);
    const uint8_t *file;
    uint64_t file_size, etext, end, page;
    struct area *image = nullptr, *stack = nullptr;
    struct process *p;
    struct task *t = nullptr;

    if (!img)
        return nullptr;
    file = img->start;
    file_size = (uint64_t)(img->end - img->start);
    if (file_size < 24 || get_le32(file + 4) != IMAGE_MAGIC)
        return nullptr;
    etext = get_le64(file + 8);
    end = get_le64(file + 16);
    if (etext > end || file_size > end || etext % PAGE_SIZE)
        return nullptr;

    if (!(page = pmm_alloc()))
        return nullptr;
    p = phys_to_virt(page);
    /* Two references: the watcher's, and its own while it lives. */
    *p = (struct process){ .name = img->name, .pri = pri, .affinity = affinity,
                           .watcher = watcher, .watch_task = watch_task, .watch_sig = sig,
                           .refs = 2, .klog_tokens = 32, .klog_at = time_now() };
    if (!arch_domain_create(&p->domain)) {
        pmm_free(page);
        return nullptr;
    }
    atomic_fetch_add(&live_records, 1);

    /* One Area for the image, so code and data stay where PC-relative
     * code expects them: code and constants read and execute, then data
     * and .bss read and write. */
    image = area_new((end + PAGE_SIZE - 1) / PAGE_SIZE, AREAF_EXEC, false);
    if (image) {
        area_copy_in(image, 0, file, file_size);
        for (uint64_t off = 0; off < etext; off += PAGE_SIZE)
            arch_sync_icache(phys_to_virt(area_pa(image, off)), PAGE_SIZE);
        if (!area_map(image, p, AREAR_READ | AREAR_WRITE, etext)) {
            area_destroy(image);
            image = nullptr;
        }
    }
    if (image && new_stack(p, &stack)) {
        p->stack_base = area_base(stack);
        t = new_user_task(p, area_base(image), stack, arg, 0);
    }
    if (!t) {
        area_process_died(p);
        arch_domain_destroy(&p->domain);
        atomic_store(&p->refs, 1);
        process_free(p);
        return nullptr;
    }
    if (held)
        p->held = t;
    else
        launch(p, t);
    return p;
}

struct process *process_create(const char *name, int pri, uint64_t arg,
                               struct task *watcher, uint64_t sig)
{
    return create(name, pri, arg, watcher, 0, sig, TASK_ANY_CPU, false);
}

struct process *process_create_on(const char *name, int pri, uint64_t arg,
                                  struct task *watcher, uint64_t sig, uint64_t affinity)
{
    return create(name, pri, arg, watcher, 0, sig, affinity, false);
}

/* --- Ending. --- */

/* Mark every task of 'p' but 'except' killed and make it notice. */
static void kill_tasks(struct process *p, struct task *except)
{
    unsigned long was = spin_lock_irqsave(&p->lock);

    atomic_store(&p->dying, true);
    for (struct task *t = p->tasks; t; t = t->next_in_proc) {
        if (t == except || atomic_exchange(&t->killed, true))
            continue;
        task_signal(t, SIGF_KILL);      /* out of Wait */
        if (t->state == TASK_RUNNING && t->cpu != this_cpu())
            arch_send_ipi(t->cpu);      /* out of user mode */
    }
    spin_unlock_irqrestore(&p->lock, was);
}

/* The calling task leaves its process for good. */
[[noreturn]] static void leave(void)
{
    struct task *self = task_self();
    struct process *p = self->process;
    unsigned long was;
    struct task **pp;

    ipc_task_gone(self);        /* its ports and connections close */
    ktimer_stop(&self->alarm);
    was = spin_lock_irqsave(&p->lock);
    for (pp = &p->tasks; *pp != self; pp = &(*pp)->next_in_proc)
        ;
    *pp = self->next_in_proc;
    p->ntasks--;
    atomic_fetch_add(&p->calls_done, atomic_load(&self->calls));
    spin_unlock_irqrestore(&p->lock, was);
    handle_free(self->handle);
    /* Its stack goes now, unless it's the last task: then the whole
     * process goes once it has switched away (its stack is in use until
     * then, and the domain with it). */
    if (p->tasks)
        area_release(self->user_stack, p);
    task_exit();                /* finish_switch then calls process_task_finished */
}

/* A held process's first task, never started: it goes without running,
 * and the process with it. */
static void discard_held(struct process *p, struct task *t)
{
    atomic_store(&p->dying, true);
    handle_free(t->handle);
    task_discard(t);            /* its stack goes with the process's Areas */
    reclaim(p);
}

void process_kill(struct process *p)
{
    struct task *held = __atomic_exchange_n(&p->held, nullptr, __ATOMIC_ACQ_REL);

    if (!atomic_load(&p->dying))
        atomic_store(&p->killed, true);
    if (held)
        discard_held(p, held);
    else if (!atomic_load(&p->dying))
        kill_tasks(p, nullptr);
}

void process_release(struct process *p)
{
    if (!atomic_load(&p->dead))
        process_kill(p);
    process_put(p);
}

static void set_result(struct process *p, int64_t code, enum fault kind, uint64_t pc,
                       uint64_t addr)
{
    bool mapped = kind != FAULT_NONE && arch_lookup(&p->domain, addr & ~(PAGE_SIZE - 1)) != 0;
    unsigned long was = spin_lock_irqsave(&p->lock);

    if (!atomic_load(&p->dying)) {
        p->exit_code = code;
        p->fault = kind;
        p->fault_mapped = mapped;
        p->fault_pc = pc;
        p->fault_addr = addr;
    }
    spin_unlock_irqrestore(&p->lock, was);
}

[[noreturn]] void process_exit(int64_t code)
{
    struct process *p = task_self()->process;

    set_result(p, code, FAULT_NONE, 0, 0);
    kill_tasks(p, task_self());
    leave();
}

[[noreturn]] void process_fault(enum fault kind, uint64_t pc, uint64_t addr)
{
    struct process *p = task_self()->process;

    kprintf("planar: %s: %s %p (pc %p)\n", p->name, fault_names[kind], (void *)addr,
            (void *)pc);
    set_result(p, -1, kind, pc, addr);
    kill_tasks(p, task_self());
    leave();
}

int64_t process_create_task(uint64_t entry, uint64_t arg)
{
    struct process *p = task_self()->process;
    struct area *stack;
    struct task *t;
    Handle h;

    if (__atomic_load_n(&p->ntasks, __ATOMIC_RELAXED) >= PROCESS_TASKS)
        return ERR_LIMIT;       /* early, cheap; launch() decides */
    if (!new_stack(p, &stack))
        return ERR_NOMEM;
    if (!(t = new_user_task(p, entry, stack, arg, 0))) {
        area_release(stack, p);
        return handle_error(p);
    }
    h = t->handle;              /* once launched, it may end at once */
    int64_t err = launch(p, t);

    return err ? err : h;
}

[[noreturn]] void process_exit_task(void)
{
    leave();
}

/* --- Processes from user mode. --- */

static void signal_fn(void *obj, void *arg)
{
    task_signal(obj, *(uint64_t *)arg);
}

static void signal_task_handle(Handle h, uint64_t sig)
{
    handle_with(h, HANDLE_TASK, signal_fn, &sig);
}

int64_t process_create_call(uint64_t user_name, uint64_t arg, int64_t sigbit, uint32_t flags)
{
    struct task *self = task_self();
    struct process *parent = self->process, *p;
    char name[NAME_MAX];
    Handle h;

    if (!syscall_copy_string(name, user_name, sizeof name))
        return ERR_BADADDR;
    if (!syscall_sigbit(sigbit) || (flags & ~PROCF_HELD))
        return ERR_BADARG;
    if (!find_image(name))
        return ERR_NOTFOUND;
    if (!(p = create(name, parent->pri, arg, nullptr, self->handle, 1ULL << sigbit,
                     parent->affinity, true)))
        return ERR_NOMEM;
    if (!(h = handle_new(HANDLE_PROCESS, p, parent))) {
        int64_t err = handle_error(parent);

        process_release(p);
        return err;
    }
    /* From here a sibling task may close the handle under us; the
     * process is then killed, and starting it does nothing. */
    if (!(flags & PROCF_HELD))
        process_start_call(h);
    return h;
}

static void hold_fn(void *obj, void *arg)
{
    process_get(obj);
    *(struct process **)arg = obj;
}

/* The process 'h' names, held, if the caller's process owns the handle. */
static struct process *get_child(Handle h)
{
    struct process *p = nullptr;

    if (!handle_with(h, HANDLE_PROCESS, hold_fn, &p))
        return nullptr;
    if (handle_get(h, HANDLE_PROCESS, task_self()->process) != p) {
        process_put(p);
        return nullptr;
    }
    return p;
}

int64_t process_start_call(uint32_t h)
{
    struct process *p = get_child(h);
    struct task *t;

    if (!p)
        return ERR_BADHANDLE;
    t = __atomic_exchange_n(&p->held, nullptr, __ATOMIC_ACQ_REL);
    if (t && launch(p, t) != 0)
        reclaim(p);             /* killed while starting: it never ran */
    process_put(p);
    return t ? 0 : ERR_EXISTS;
}

int64_t process_kill_call(uint32_t h)
{
    struct process *p = get_child(h);

    if (!p)
        return ERR_BADHANDLE;
    process_kill(p);
    process_put(p);
    return 0;
}

int64_t process_query_call(uint32_t h, uint64_t info_out)
{
    struct process *p = get_child(h);
    struct ProcessInfo info = { 0 };

    if (!p)
        return ERR_BADHANDLE;
    if (__atomic_load_n(&p->held, __ATOMIC_ACQUIRE))
        info.pi_State = PROCS_HELD;
    else if (!atomic_load(&p->dead))
        info.pi_State = PROCS_RUNNING;
    else if (atomic_load(&p->killed))
        info.pi_State = PROCS_KILLED;
    else if (p->fault != FAULT_NONE)
        info.pi_State = PROCS_FAULTED;
    else
        info.pi_State = PROCS_EXITED;
    if (info.pi_State >= PROCS_EXITED) {
        info.pi_ExitCode = p->exit_code;
        info.pi_Fault = p->fault;
        info.pi_FaultAddress = p->fault_addr;
        info.pi_FaultPC = p->fault_pc;
    }
    process_put(p);
    return arch_copy_to_user(info_out, &info, sizeof info) ? 0 : ERR_BADADDR;
}

void user_return(void)
{
    if (atomic_load_explicit(&task_self()->killed, memory_order_relaxed))   /* a hint: the kill signals too */
        leave();
}

/*
 * Every task of 'p' has switched away for the last time, so no CPU has
 * its domain loaded and nothing runs in it: give everything back.
 * Called from finish_switch, interrupts off.
 */
static void reclaim(struct process *p)
{
    struct task *watcher = p->watcher;
    Handle watch_task = p->watch_task;
    uint64_t sig = p->watch_sig;

    /* Handles first: once its Areas go, no handle may still find them.
     * The processes it created die with it. */
    handle_free_owner(p);
    area_process_died(p);
    arch_domain_destroy(&p->domain);
    /* Its own reference, before 'dead': from then on the watcher may
     * let the record go. */
    process_put(p);
    atomic_store(&p->dead, true);
    if (watcher)
        task_signal(watcher, sig);
    else if (watch_task)
        signal_task_handle(watch_task, sig);
}

void process_task_finished(struct process *p)
{
    if (atomic_fetch_sub(&p->unfinished, 1) == 1)
        reclaim(p);
}

void process_get(struct process *p)
{
    atomic_fetch_add(&p->refs, 1);
}

void process_put(struct process *p)
{
    if (atomic_fetch_sub(&p->refs, 1) == 1) {
        pmm_free(virt_to_phys(p));
        atomic_fetch_sub(&live_records, 1);
    }
}

void process_free(struct process *p)
{
    process_put(p);
}

uint64_t process_calls(struct process *p)
{
    unsigned long was = spin_lock_irqsave(&p->lock);
    uint64_t n = atomic_load(&p->calls_done);

    for (struct task *t = p->tasks; t; t = t->next_in_proc)
        n += atomic_load_explicit(&t->calls, memory_order_relaxed);
    spin_unlock_irqrestore(&p->lock, was);
    return n;
}

unsigned process_count(void)
{
    return atomic_load(&live_records);
}
