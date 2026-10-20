/*
 * kernel/process.c -- processes: protection domains that own resources
 */
#include <planar/endian.h>
#include <planar/syscalls.h>

#include "cpu.h"
#include "handle.h"
#include "kprintf.h"
#include "kstring.h"
#include "pmm.h"
#include "process.h"
#include "sched.h"
#include "userimg.h"

constexpr uint32_t IMAGE_MAGIC = 0x524e4c50;    /* "PLNR" */
constexpr unsigned STACK_PAGES = 4;             /* 16 KiB user stacks */

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

/*
 * 'pages' of zeroed, physically contiguous memory, recorded so the
 * process gives it back, and identity-mapped (VA = PA) with 'flags' up
 * to 'split' bytes and 'flags2' after. 0 if there's no memory.
 */
static uint64_t region_alloc(struct process *p, uint32_t pages, uint32_t flags,
                             uint64_t split, uint32_t flags2)
{
    uint64_t pa = pmm_alloc_contig(pages);

    if (!pa)
        return 0;
    unsigned long was = spin_lock_irqsave(&p->lock);
    if (p->nregions == PROCESS_REGIONS) {
        spin_unlock_irqrestore(&p->lock, was);
        for (uint32_t i = 0; i < pages; i++)
            pmm_free(pa + i * PAGE_SIZE);
        return 0;
    }
    p->regions[p->nregions++] = (struct region){ .va = pa, .pa = pa, .pages = pages };
    spin_unlock_irqrestore(&p->lock, was);
    for (uint32_t i = 0; i < pages; i++) {
        uint64_t off = (uint64_t)i * PAGE_SIZE;

        if (!arch_map(&p->domain, pa + off, pa + off, off < split ? flags : flags2))
            return 0;           /* the region is recorded: reclaim frees it */
    }
    return pa;
}

static void free_regions(struct process *p)
{
    for (unsigned r = 0; r < p->nregions; r++)
        for (uint32_t i = 0; i < p->regions[r].pages; i++)
            pmm_free(p->regions[r].pa + (uint64_t)i * PAGE_SIZE);
    p->nregions = 0;
}

/* --- Tasks of a process. --- */

static void user_start(void *)
{
    struct task *self = task_self();

    user_return();              /* killed before it ever ran */
    arch_user_enter(self->user_pc, self->user_sp, self->user_arg[0], self->user_arg[1]);
}

/* A task in 'p' starting at 'pc' with 'sp', not yet launched. */
static struct task *new_user_task(struct process *p, uint64_t pc, uint64_t sp,
                                  uint64_t arg0, uint64_t arg1)
{
    struct task *t = task_new(p->name, p->pri, TASK_ANY_CPU, user_start, nullptr);

    if (!t)
        return nullptr;
    t->process = p;
    t->domain = &p->domain;
    t->user_pc = pc;
    t->user_sp = sp;
    t->user_arg[0] = arg0;
    t->user_arg[1] = arg1;
    t->handle = handle_new(HANDLE_TASK, t, p);
    if (!t->handle) {
        task_discard(t);
        return nullptr;
    }
    return t;
}

/* Add a new task to 'p' and start it; false if 'p' is ending. */
static bool launch(struct process *p, struct task *t)
{
    unsigned long was = spin_lock_irqsave(&p->lock);

    if (atomic_load(&p->dying)) {
        spin_unlock_irqrestore(&p->lock, was);
        handle_free(t->handle);
        task_discard(t);
        return false;
    }
    t->next_in_proc = p->tasks;
    p->tasks = t;
    atomic_fetch_add(&p->unfinished, 1);
    spin_unlock_irqrestore(&p->lock, was);
    task_launch(t);
    return true;
}

struct process *process_create(const char *name, int pri, uint64_t arg,
                               struct task *watcher, uint64_t sig)
{
    const struct user_image *img = find_image(name);
    const uint8_t *file;
    uint64_t file_size, etext, end, base, stack, page;
    struct process *p;
    struct task *t;

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
    *p = (struct process){ .name = img->name, .pri = pri, .watcher = watcher,
                           .watch_sig = sig };
    if (!arch_domain_create(&p->domain)) {
        pmm_free(page);
        return nullptr;
    }
    atomic_fetch_add(&live_records, 1);

    /* Code and constants RX, then data and .bss RW. */
    base = region_alloc(p, (uint32_t)((end + PAGE_SIZE - 1) / PAGE_SIZE),
                        MAP_USER | MAP_READ | MAP_EXEC, etext, MAP_USER | MAP_READ | MAP_WRITE);
    stack = region_alloc(p, STACK_PAGES, MAP_USER | MAP_READ | MAP_WRITE, ~0ULL, 0);
    if (base && stack) {
        memcpy(phys_to_virt(base), file, file_size);
        arch_sync_icache(phys_to_virt(base), file_size);
    }
    t = base && stack ? new_user_task(p, base, stack + STACK_PAGES * PAGE_SIZE, arg, 0)
                      : nullptr;
    if (!t) {
        free_regions(p);
        arch_domain_destroy(&p->domain);
        process_free(p);
        return nullptr;
    }
    launch(p, t);
    return p;
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
    unsigned long was = spin_lock_irqsave(&p->lock);
    struct task **pp;

    for (pp = &p->tasks; *pp != self; pp = &(*pp)->next_in_proc)
        ;
    *pp = self->next_in_proc;
    spin_unlock_irqrestore(&p->lock, was);
    handle_free(self->handle);
    task_exit();                /* finish_switch then calls process_task_finished */
}

void process_kill(struct process *p)
{
    if (!atomic_load(&p->dying))
        kill_tasks(p, nullptr);
}

static void set_result(struct process *p, int64_t code, enum fault kind, uint64_t pc,
                       uint64_t addr)
{
    unsigned long was = spin_lock_irqsave(&p->lock);

    if (!atomic_load(&p->dying)) {
        p->exit_code = code;
        p->fault = kind;
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
    uint64_t stack = region_alloc(p, STACK_PAGES, MAP_USER | MAP_READ | MAP_WRITE, ~0ULL, 0);
    struct task *t;

    if (!stack)
        return ERR_NOMEM;
    t = new_user_task(p, entry, stack + STACK_PAGES * PAGE_SIZE, arg, 0);
    if (!t)
        return ERR_NOMEM;
    if (!launch(p, t))
        return ERR_NOTFOUND;
    return t->handle;
}

[[noreturn]] void process_exit_task(void)
{
    leave();
}

void user_return(void)
{
    if (atomic_load(&task_self()->killed))
        leave();
}

/*
 * Every task of 'p' has switched away for the last time, so no CPU has
 * its domain loaded and nothing runs in it: give everything back.
 * Called from finish_switch, interrupts off.
 */
static void reclaim(struct process *p)
{
    free_regions(p);
    arch_domain_destroy(&p->domain);
    handle_free_owner(p);
    atomic_store(&p->dead, true);
    if (p->watcher)
        task_signal(p->watcher, p->watch_sig);
}

void process_task_finished(struct process *p)
{
    if (atomic_fetch_sub(&p->unfinished, 1) == 1)
        reclaim(p);
}

void process_free(struct process *p)
{
    pmm_free(virt_to_phys(p));
    atomic_fetch_sub(&live_records, 1);
}

unsigned process_count(void)
{
    return atomic_load(&live_records);
}
