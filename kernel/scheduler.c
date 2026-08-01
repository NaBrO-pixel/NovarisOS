/* scheduler.c - Milestone 9: real process structures + a round-robin
 * preemptive scheduler.
 *
 * The core trick: a process that has never run yet, and a process that
 * got preempted mid-flight, can be resumed through *exactly* the same
 * code path if we make "never run yet" look like "was just interrupted".
 * isr.s's shared stub epilogue (the tail end of isr_common_stub and
 * irq_common_stub) always does the same thing to return to ring 3:
 *
 *     pop gs; pop fs; pop es; pop ds
 *     popa             ; edi,esi,ebp,(esp slot, discarded),ebx,edx,ecx,eax
 *     add esp, 8        ; skip int_no/err_code
 *     iret              ; pops eip,cs,eflags,useresp,ss
 *
 * That's a fixed layout (see registers_t in idt.h). So instead of only
 * ever restoring a *real* previously-saved frame, scheduler_spawn_flat()
 * synthesizes one by hand at the top of a fresh kernel stack, with eip =
 * the program's entry point and useresp/ss pointing at its user stack.
 * From the epilogue's point of view a brand new process and a resumed
 * one are indistinguishable - both are just "an esp value pointing at a
 * well-formed frame".
 *
 * The actual stack switch happens in isr.s itself (see the
 * `scheduler_next_esp` hook added to both stub epilogues there), not
 * here - by the time this file changes which process is "current" and
 * writes scheduler_next_esp, we're still running on the *old* process's
 * kernel stack, several C stack frames deep (syscall/irq handler ->
 * isr_handler/irq_handler -> isr_common_stub). We can't safely repoint
 * ESP out from under a live C call chain ourselves; instead we let that
 * whole chain unwind normally back to the raw asm epilogue, which is the
 * one place it's safe to swap stacks, and *it* honors the hook we left. */

#include "scheduler.h"
#include "paging.h"
#include "pmm.h"
#include "kheap.h"
#include "gdt.h"
#include "console.h"
#include "fpu.h"
#include "pit.h"
#include "posix.h"   /* EAGAIN, ETIMEDOUT - the values a woken waiter gets */

#define MAX_PROCESSES 8
/* 8KB, not 4KB. Milestone 17 runs Win32 programs as scheduler tasks, and
 * their kernel-side call chains are far deeper than a demo task's
 * sys_write: int 0x81 -> win32_dispatch -> printf -> the format engine ->
 * the float renderer, plus a ring-3 callback's borrowed slice on top. */
#define PROC_KSTACK_SIZE 8192u
#define PAGE_SIZE 4096u

/* Preempt every this-many PIT ticks. At the 100Hz pit_install(100) rate
 * kernel_main() uses, 2 ticks = ~20ms time slices - frequent enough that
 * a handful of tight-looping demo tasks visibly interleave their output
 * within a couple seconds, without spending most of the CPU budget on
 * scheduling overhead itself. */
#define SCHED_TICKS_PER_SLICE 2

static process_t process_table[MAX_PROCESSES];
static int process_count = 0;
static int next_pid = 1;

static process_t* current = 0;
static int active = 0;
static uint32_t tick_countdown = SCHED_TICKS_PER_SLICE;

/* Consumed by the epilogue hook added to isr.s's isr_common_stub /
 * irq_common_stub: non-zero means "switch to this kernel esp instead of
 * continuing on whatever's currently on the stack" (see the file-level
 * comment above). The asm clears it back to 0 itself once consumed. */
uint32_t scheduler_next_esp = 0;

/* Implemented in kernel/scheduler_asm.s. Mirrors process_asm.s's
 * process_run_user_mode()/process_exit_to_kernel() pair, but for
 * "run every spawned process to completion" instead of just one:
 * scheduler_bootstrap_save_and_jump() saves the caller's kernel esp/ebp
 * (into scheduler_resume_esp/ebp below) and jumps into the first
 * process's synthetic frame; scheduler_return_to_caller() unwinds back
 * to right after that call once the last process has exited. */
extern void scheduler_bootstrap_save_and_jump(uint32_t first_esp);
extern void scheduler_return_to_caller(void) __attribute__((noreturn));
uint32_t scheduler_resume_esp = 0;
uint32_t scheduler_resume_ebp = 0;

void scheduler_init(void) {
    process_count = 0;
    next_pid = 1;
    current = 0;
    active = 0;
    tick_countdown = SCHED_TICKS_PER_SLICE;
    scheduler_next_esp = 0;
}

int scheduler_is_active(void) {
    return active;
}

static void copy_name(process_t* p, const char* name) {
    int i = 0;
    while (name[i] && i < PROCESS_MAX_NAME - 1) {
        p->name[i] = name[i];
        i++;
    }
    p->name[i] = '\0';
}

/* The part every spawn shares: a kernel stack, a one-page user stack, and
 * a synthetic trap frame that makes a never-run task indistinguishable
 * from a preempted one. `entry` is where ring 3 starts; `load_pages` is
 * how many image pages this task is responsible for freeing (0 for a
 * thread, which loaded nothing of its own). Everything it maps goes into
 * whatever address space is current, which is how one function serves
 * processes and threads alike. */
static int spawn_common(const char* name, uint32_t entry, uint32_t load_vaddr,
                        uint32_t load_pages, uint32_t stack_top,
                        int owns_pd, uint32_t teb, int map_stack) {
    /* A Win32 thread arrives with its stack already mapped - out of the
     * emulation layer's own arena, which is freed wholesale when the
     * program exits - so it asks for no stack page here and owns none. */
    if (map_stack) {
        uint32_t stack_phys = pmm_alloc_frame();
        if (!stack_phys) return -1;
        paging_map_page(stack_top - PAGE_SIZE, stack_phys,
                         PAGE_PRESENT | PAGE_RW | PAGE_USER);
    }

    uint8_t* kstack_base = (uint8_t*)kmalloc(PROC_KSTACK_SIZE);
    if (!kstack_base) return -1;
    uint32_t kstack_top = (uint32_t)kstack_base + PROC_KSTACK_SIZE;

    /* FXSAVE needs 16-byte alignment, which kmalloc does not promise, so
     * over-allocate and align by hand. The unaligned base is what gets
     * freed. */
    uint8_t* fpu_raw = (uint8_t*)kmalloc(sizeof(fpu_state_t) + 16);
    if (!fpu_raw) { kfree(kstack_base); return -1; }
    fpu_state_t* fpu = (fpu_state_t*)(((uint32_t)fpu_raw + 15u) & ~15u);
    fpu_state_init(fpu);

    /* Build the synthetic initial trap frame - see the file-level
     * comment for why this makes a never-run process look, to the
     * switch code, just like one that was preempted mid-flight. */
    registers_t* frame = (registers_t*)(kstack_top - sizeof(registers_t));
    frame->ds = 0x23;               /* user data selector | RPL3 */
    frame->es = 0x23;
    /* A task with a TEB runs with fs addressing it (selector 0x33, GDT
     * entry 6), the same arrangement process_asm.s sets up for a
     * single Win32 program - see gdt_set_teb(). Everything else gets the
     * flat user data selector. */
    frame->fs = teb ? 0x33 : 0x23;
    frame->gs = 0x23;
    frame->edi = 0;
    frame->esi = 0;
    frame->ebp = 0;
    frame->esp_dummy = 0;           /* popa discards this slot */
    frame->ebx = 0;
    frame->edx = 0;
    frame->ecx = 0;
    frame->eax = 0;
    frame->int_no = 0;              /* skipped by `add esp,8`, value unused */
    frame->err_code = 0;
    frame->eip = entry;             /* where ring 3 starts */
    frame->cs = 0x1B;               /* user code selector | RPL3 */
    frame->eflags = 0x202;          /* IF=1 (so this task can itself be
                                      * preempted), reserved bit 1 set */
    frame->useresp = stack_top;
    frame->ss = 0x23;

    process_t* p = &process_table[process_count++];
    p->pid = next_pid++;
    copy_name(p, name);
    p->esp = (uint32_t)frame;
    p->kernel_stack_base = (uint32_t)kstack_base;
    p->fpu_base = (uint32_t)fpu_raw;
    p->fpu = fpu;
    p->kernel_stack_top = kstack_top;
    p->load_vaddr = load_vaddr;
    p->load_pages = load_pages;
    p->stack_top = stack_top;
    p->page_directory = paging_current_address_space();
    p->owns_page_directory = owns_pd;
    p->teb = teb;
    p->owns_stack = map_stack;
    p->tls_base = 0;
    p->tls_limit = 0;
    p->clear_child_tid = 0;
    p->wait_addr = 0;
    p->wait_deadline = 0;
    p->wait_retry = 0;
    p->state = PROC_READY;
    return p->pid;
}

/* Maps and copies a flat image into the current address space. Returns
 * the page count, or 0 if physical memory ran out partway (the pages it
 * did map are left for the reaper to free). */
static uint32_t load_image(const uint8_t* image, uint32_t size,
                           uint32_t load_vaddr) {
    uint32_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    if (pages == 0) pages = 1;

    for (uint32_t i = 0; i < pages; i++) {
        uint32_t phys = pmm_alloc_frame();
        if (!phys) return 0;
        paging_map_page(load_vaddr + i * PAGE_SIZE, phys,
                         PAGE_PRESENT | PAGE_RW | PAGE_USER);
    }

    uint8_t* dest = (uint8_t*)load_vaddr;
    for (uint32_t i = 0; i < size; i++) dest[i] = image[i];
    return pages;
}

int scheduler_spawn_flat(const char* name, const uint8_t* image, uint32_t size,
                          uint32_t load_vaddr, uint32_t stack_top) {
    if (process_count >= MAX_PROCESSES) return -1;

    uint32_t pages = load_image(image, size, load_vaddr);
    if (!pages) return -1;

    return spawn_common(name, load_vaddr, load_vaddr, pages, stack_top, 0, 0, 1);
}

int scheduler_spawn_process(const char* name, const uint8_t* image,
                            uint32_t size, uint32_t load_vaddr,
                            uint32_t stack_top) {
    if (process_count >= MAX_PROCESSES) return -1;

    uint32_t caller_as = paging_current_address_space();
    uint32_t as = paging_create_address_space();
    if (!as) return -1;

    /* Load into the new address space by standing in it. Every mapping
     * spawn_common() and load_image() make goes through the ordinary
     * paging calls, which act on whatever is in CR3 - so switching here
     * is the entire mechanism, and neither of them needs to know that
     * address spaces exist. Safe because the kernel half is identical in
     * both: the image bytes are read out of a kernel buffer that is at
     * the same address either side of the switch. */
    paging_switch_address_space(as);

    uint32_t pages = load_image(image, size, load_vaddr);
    int pid = pages ? spawn_common(name, load_vaddr, load_vaddr, pages,
                                   stack_top, 1, 0, 1)
                    : -1;

    paging_switch_address_space(caller_as);

    if (pid < 0) paging_destroy_address_space(as);
    return pid;
}

int scheduler_spawn_posix_thread(const char* name, uint32_t entry,
                                 uint32_t esp, uint32_t tls_base,
                                 uint32_t tls_limit,
                                 uint32_t clear_child_tid) {
    if (process_count >= MAX_PROCESSES) return -1;
    int pid = spawn_common(name, entry, entry, 0, esp, 0, 0, 0);
    if (pid < 0) return -1;

    process_t* p = &process_table[process_count - 1];
    p->tls_base = tls_base;
    p->tls_limit = tls_limit;
    p->clear_child_tid = clear_child_tid;

    /* clone() returns 0 in the child and the tid in the parent, and the
     * synthetic frame is the child's whole initial state - so eax is set
     * here rather than anywhere the child could observe being changed. */
    registers_t* frame = (registers_t*)p->esp;
    frame->eax = 0;
    if (tls_base) frame->gs = 0x3B; /* GDT entry 7 | RPL 3 */
    return pid;
}

void scheduler_set_current_tls(uint32_t base, uint32_t limit) {
    if (!current) return;
    current->tls_base = base;
    current->tls_limit = limit;
}

uint32_t scheduler_current_clear_child_tid(void) {
    return current ? current->clear_child_tid : 0;
}

void scheduler_run_single(const char* name, uint32_t entry, uint32_t esp) {
    /* map_stack 0: the caller has already mapped the stack this esp points
     * into, and owns it. */
    if (spawn_common(name, entry, entry, 0, esp, 0, 0, 0) < 0) return;
    scheduler_run_until_idle();
}

int scheduler_spawn_win32_thread(const char* name, uint32_t entry,
                                 uint32_t esp, uint32_t teb) {
    if (process_count >= MAX_PROCESSES) return -1;
    /* map_stack 0: the Win32 layer allocated this thread's stack out of
     * its own arena and frees it wholesale when the program exits, so the
     * reaper must keep its hands off it. `esp` is a fully-built initial
     * stack pointer (argument and return address already pushed), not
     * merely the top of a blank page. */
    return spawn_common(name, entry, entry, 0, esp, 0, teb, 0);
}

int scheduler_spawn_thread(const char* name, uint32_t entry,
                           uint32_t stack_top) {
    if (process_count >= MAX_PROCESSES) return -1;
    /* No image, no address space: load_pages 0 says "I own no code", and
     * owns_pd 0 says "the directory I run in is somebody else's". What is
     * left - a stack and a register context - is exactly what a thread
     * is. */
    return spawn_common(name, entry, entry, 0, stack_top, 0, 0, 1);
}

/* Round robin: scans forward from just after `current`'s slot, wrapping,
 * returning the first runnable process found (which may be `current`
 * itself, if it's the only one left - callers that just marked `current`
 * a zombie rely on that state already being set before this runs, so it
 * can't be picked again). Returns 0 if nothing at all can run.
 *
 * Since Milestone 25 a task can also be PROC_BLOCKED, which is skipped
 * the same way a zombie is. That introduces a case that could not happen
 * before: every remaining task alive but parked, with nobody left to wake
 * anyone. That is a deadlock in the *program*, and the kernel's job is to
 * not make it a deadlock in the machine - so rather than returning 0 and
 * letting a caller unwind past tasks that are still alive, every blocked
 * task is woken with -EAGAIN. Every futex caller already handles that
 * (it means "the word changed under you, look again"), so the program
 * degrades to the spin it would have done before this milestone instead
 * of vanishing. */
static int wake_every_blocked(int32_t result);

static process_t* scan_runnable(int cur_idx) {
    for (int off = 1; off <= process_count; off++) {
        int idx = (cur_idx + off) % process_count;
        process_state_t st = process_table[idx].state;
        if (st != PROC_ZOMBIE && st != PROC_BLOCKED) {
            return &process_table[idx];
        }
    }
    return 0;
}

static process_t* pick_next_ready(void) {
    if (process_count == 0) return 0;

    int cur_idx = 0;
    for (int i = 0; i < process_count; i++) {
        if (&process_table[i] == current) {
            cur_idx = i;
            break;
        }
    }

    process_t* next = scan_runnable(cur_idx);
    if (next) return next;

    /* Nothing runnable. If anything is merely blocked, this is the
     * deadlock case above rather than the end of the batch. */
    if (wake_every_blocked(-EAGAIN) > 0) return scan_runnable(cur_idx);
    return 0;
}

static void switch_to(process_t* next);

void scheduler_tick(registers_t* regs) {
    if (!active || !current) return;

    /* Timed futex waits expire here, and deliberately before the
     * ring-3 check below: a task whose deadline has passed has to become
     * runnable again whether or not this particular tick is allowed to
     * preempt anything. */
    scheduler_expire_waits(pit_get_ticks(), -ETIMEDOUT);

    /* Only preempt ring-3 code. A timer that lands while this task is
     * inside a syscall must not switch away: `regs` would be a same-
     * privilege interrupt frame, which has no useresp/ss on it at all, so
     * the registers_t the resume path expects would be two fields short
     * and resuming it would return to garbage. It also would not be
     * *useful* - the task's kernel-side C call chain cannot be resumed
     * from a trap frame anyway.
     *
     * This was latent until Milestone 17. Before it, the only tasks the
     * scheduler ran were tiny demo programs whose syscalls never
     * re-enabled interrupts; now it runs Win32 programs, and Sleep()
     * deliberately does exactly that. */
    if ((regs->cs & 3) != 3) return;

    if (tick_countdown > 0) {
        tick_countdown--;
        return;
    }
    tick_countdown = SCHED_TICKS_PER_SLICE;

    current->esp = (uint32_t)regs;
    current->state = PROC_READY;

    process_t* next = pick_next_ready();
    if (!next) {
        /* Shouldn't happen while active (there'd be nothing left to
         * preempt), but stay safe rather than switch to nothing. */
        current->state = PROC_RUNNING;
        return;
    }

    switch_to(next);
}

/* Makes `next` the running task. The CR3 load is the Milestone 16
 * addition, and it is safe to do here - several C frames deep, on the
 * outgoing task's kernel stack - precisely because Milestone 14 made the
 * kernel half identical in every address space: this code, this stack and
 * the frame `scheduler_next_esp` points at are all at the same addresses
 * before and after the load. Only the user half changes, and nothing on
 * the switch path touches that.
 *
 * Skipping the load when the directory is unchanged is not just an
 * optimisation - it is what makes switching between two threads of one
 * process cheap, since a CR3 write flushes the whole TLB. */
static void switch_to(process_t* next) {
    /* x87 and SSE are per-thread state exactly as the general registers
     * are, and unlike them the interrupt stub does not touch them - so
     * they are saved and restored here or not at all. */
    if (current && current->fpu) fpu_save(current->fpu);
    if (next->fpu) fpu_restore(next->fpu);

    if (next->page_directory != current->page_directory) {
        paging_switch_address_space(next->page_directory);
    }
    /* Each Win32 thread has its own TEB, and fs is a segment whose *base*
     * is that TEB - so the GDT descriptor has to be repointed on every
     * switch between threads, not just once per program. Getting this
     * wrong would give two threads one errno and one SEH chain. */
    if (next->teb) gdt_set_teb(next->teb, PAGE_SIZE - 1);
    /* Same reasoning for POSIX thread-local storage, which lives behind
     * gs rather than fs - see gdt_set_tls(). */
    if (next->tls_base) gdt_set_tls(next->tls_base, next->tls_limit);
    current = next;
    current->state = PROC_RUNNING;
    gdt_set_kernel_stack(current->kernel_stack_top);
    scheduler_next_esp = current->esp;
}

/* Frees every process's kernel stack and mapped user pages, then resets
 * the table. Only ever called after every process is a zombie and we've
 * fully unwound off their kernel stacks (from scheduler_run_until_idle,
 * back on the original caller's own stack) - never from inside a
 * process's own exit path, which would mean freeing memory out from
 * under code still (about to be) executing on it. */
/* Unmaps a page *and* hands its frame back to the PMM. Unmapping alone
 * only removes the translation - the frame stays marked in use forever,
 * which is how `multitask` came to lose six frames every time it ran.
 * Found while checking that Milestone 15's address spaces gave back
 * everything they took; the bug itself dates from Milestone 9. */
static void free_user_page(uint32_t virt) {
    uint32_t pte = paging_get_entry(virt);
    paging_unmap_page(virt);
    if (pte & PAGE_PRESENT) pmm_free_frame(pte & ~0xFFFu);
}

static void reap_all(void) {
    uint32_t caller_as = paging_current_address_space();

    /* Two passes, because a task's pages can only be unmapped from inside
     * its own address space, and a directory cannot be destroyed while it
     * is the one loaded in CR3. */
    for (int i = 0; i < process_count; i++) {
        process_t* p = &process_table[i];
        paging_switch_address_space(p->page_directory);
        for (uint32_t pg = 0; pg < p->load_pages; pg++) {
            free_user_page(p->load_vaddr + pg * PAGE_SIZE);
        }
        if (p->owns_stack) free_user_page(p->stack_top - PAGE_SIZE);
        kfree((void*)p->kernel_stack_base);
        kfree((void*)p->fpu_base);
    }

    paging_switch_address_space(caller_as);

    for (int i = 0; i < process_count; i++) {
        process_t* p = &process_table[i];
        /* Only the task that created a directory destroys it. Threads
         * share a sibling's and must not, and a task spawned into a
         * directory the caller owns must leave it to the caller. */
        if (p->owns_page_directory) {
            paging_destroy_address_space(p->page_directory);
        }
    }

    process_count = 0;
}

void scheduler_exit_current(void) {
    if (!current) return;

    current->state = PROC_ZOMBIE;
    process_t* next = pick_next_ready();

    if (!next) {
        /* Last process standing just exited: nothing left to switch to.
         * Discard this entire (deeply-nested) C call chain the same way
         * process.c's process_exit_to_kernel() discards its own -
         * jump straight back to whoever called
         * scheduler_bootstrap_save_and_jump(), as if that call had just
         * returned normally. Does not return here. */
        active = 0;
        current = 0;
        scheduler_return_to_caller();
        __builtin_unreachable();
    }

    switch_to(next);
    /* Returns normally: back up through the syscall handler -> isr
     * dispatch -> isr_common_stub, whose epilogue performs the actual
     * switch once it sees scheduler_next_esp set. */
}

int scheduler_current_pid(void) {
    return current ? current->pid : 0;
}

int scheduler_task_alive(int pid) {
    for (int i = 0; i < process_count; i++) {
        if (process_table[i].pid == pid) {
            return process_table[i].state != PROC_ZOMBIE;
        }
    }
    return 0; /* unknown pid: treat as finished, not as never-finishing */
}

/* --- blocking (Milestone 25) --------------------------------------------
 *
 * Waking means two things: making the task a candidate again, and telling
 * the syscall it was parked inside what to return. The second is possible
 * because `esp` points at that task's saved trap frame, so writing eax
 * there *is* writing the syscall's return value - the same frame the
 * isr epilogue will iret from when the task next runs. */
static void wake_one(process_t* p, int32_t result) {
    if (!p->wait_retry) ((registers_t*)p->esp)->eax = (uint32_t)result;
    p->wait_addr = 0;
    p->wait_deadline = 0;
    p->wait_retry = 0;
    p->state = PROC_READY;
}

static int wake_every_blocked(int32_t result) {
    int n = 0;
    for (int i = 0; i < process_count; i++) {
        if (process_table[i].state == PROC_BLOCKED) {
            wake_one(&process_table[i], result);
            n++;
        }
    }
    return n;
}

int scheduler_block_current(registers_t* regs, uint32_t wait_addr,
                            uint32_t deadline) {
    if (!active || !current) return 0;

    /* Decide *before* changing anything: pick_next_ready() would
     * otherwise see this task as blocked, find nothing else, and trip the
     * deadlock escape on a task that was about to park legitimately. */
    process_t* next = pick_next_ready();
    if (!next || next == current) return 0;

    current->esp = (uint32_t)regs;
    current->wait_addr = wait_addr;
    current->wait_deadline = deadline;
    current->wait_retry = 0;
    current->state = PROC_BLOCKED;
    tick_countdown = SCHED_TICKS_PER_SLICE;
    switch_to(next);
    return 1;
}

int scheduler_block_current_retry(registers_t* regs, uint32_t wait_addr) {
    if (!scheduler_block_current(regs, wait_addr, 0)) return 0;
    current->wait_retry = 1;
    return 1;
}

int scheduler_wake_on(uint32_t addr, int max, int32_t result) {
    int n = 0;
    if (!addr) return 0;
    for (int i = 0; i < process_count && n < max; i++) {
        process_t* p = &process_table[i];
        if (p->state == PROC_BLOCKED && p->wait_addr == addr) {
            wake_one(p, result);
            n++;
        }
    }
    return n;
}

int scheduler_expire_waits(uint32_t now, int32_t result) {
    int n = 0;
    for (int i = 0; i < process_count; i++) {
        process_t* p = &process_table[i];
        /* Unsigned subtraction rather than `now >= deadline`, so a tick
         * counter that wraps does not turn every wait into an immediate
         * timeout for the rest of the boot. */
        if (p->state == PROC_BLOCKED && p->wait_deadline &&
            (int32_t)(now - p->wait_deadline) >= 0) {
            wake_one(p, result);
            n++;
        }
    }
    return n;
}

int scheduler_blocked_count(void) {
    int n = 0;
    for (int i = 0; i < process_count; i++) {
        if (process_table[i].state == PROC_BLOCKED) n++;
    }
    return n;
}

int scheduler_yield_would_switch(void) {
    if (!active || !current) return 0;
    process_t* next = pick_next_ready();
    return next && next != current;
}

int scheduler_yield_from_trap(registers_t* regs) {
    if (!active || !current) return 0;

    process_t* next = pick_next_ready();
    if (!next || next == current) return 0; /* nothing else can run */

    /* Exactly what scheduler_tick() does on a timer preemption, and for
     * exactly the same reason it is safe: `regs` is this task's complete
     * ring-3 state, so resuming from it later re-enters ring 3 at the
     * instruction the trap frame names. A caller that first rewinds eip
     * to re-execute its own trap therefore gets a retry loop that other
     * threads can run in the middle of - which is how waiting is
     * implemented in Milestone 17. See w32_retry_call_later(). */
    current->esp = (uint32_t)regs;
    current->state = PROC_READY;
    tick_countdown = SCHED_TICKS_PER_SLICE;
    switch_to(next);
    return 1;
}

void scheduler_terminate_all(void) {
    /* Nothing is being scheduled, so there is no saved caller stack to
     * unwind to. Returning is the only safe answer; the caller is
     * expected to have checked scheduler_is_active() first. */
    if (!active) return;
    /* Every task, including whichever one is calling. Nothing is left
     * runnable, so control goes straight back to whoever called
     * scheduler_run_until_idle(), which then reaps the batch. */
    for (int i = 0; i < process_count; i++) {
        process_table[i].state = PROC_ZOMBIE;
    }
    active = 0;
    current = 0;
    scheduler_return_to_caller();
}

void scheduler_run_until_idle(void) {
    if (process_count == 0) return;

    uint32_t caller_as = paging_current_address_space();

    active = 1;
    tick_countdown = SCHED_TICKS_PER_SLICE;
    current = &process_table[0];
    current->state = PROC_RUNNING;
    gdt_set_kernel_stack(current->kernel_stack_top);
    if (current->fpu) fpu_restore(current->fpu);
    /* The first task is entered directly rather than through switch_to(),
     * so its address space has to be loaded by hand here. */
    paging_switch_address_space(current->page_directory);

    /* Blocks here - from this function's own point of view - until every
     * spawned process has called sys_exit. See scheduler_asm.s. */
    scheduler_bootstrap_save_and_jump(current->esp);

    /* Control lands here on whichever task exited last, so CR3 still
     * holds *its* directory. reap_all() walks every address space and
     * comes back to whatever is current when it starts, so put the
     * caller's back first. */
    paging_switch_address_space(caller_as);
    reap_all();
}
