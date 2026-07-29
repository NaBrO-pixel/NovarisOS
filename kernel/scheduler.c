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

#define MAX_PROCESSES 8
#define PROC_KSTACK_SIZE 4096u
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

int scheduler_spawn_flat(const char* name, const uint8_t* image, uint32_t size,
                          uint32_t load_vaddr, uint32_t stack_top) {
    if (process_count >= MAX_PROCESSES) return -1;

    uint32_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    if (pages == 0) pages = 1;

    for (uint32_t i = 0; i < pages; i++) {
        uint32_t phys = pmm_alloc_frame();
        if (!phys) return -1;
        paging_map_page(load_vaddr + i * PAGE_SIZE, phys,
                         PAGE_PRESENT | PAGE_RW | PAGE_USER);
    }

    uint8_t* dest = (uint8_t*)load_vaddr;
    for (uint32_t i = 0; i < size; i++) dest[i] = image[i];

    uint32_t stack_phys = pmm_alloc_frame();
    if (!stack_phys) return -1;
    paging_map_page(stack_top - PAGE_SIZE, stack_phys,
                     PAGE_PRESENT | PAGE_RW | PAGE_USER);

    uint8_t* kstack_base = (uint8_t*)kmalloc(PROC_KSTACK_SIZE);
    if (!kstack_base) return -1;
    uint32_t kstack_top = (uint32_t)kstack_base + PROC_KSTACK_SIZE;

    /* Build the synthetic initial trap frame - see the file-level
     * comment for why this makes a never-run process look, to the
     * switch code, just like one that was preempted mid-flight. */
    registers_t* frame = (registers_t*)(kstack_top - sizeof(registers_t));
    frame->ds = 0x23;               /* user data selector | RPL3 */
    frame->es = 0x23;
    frame->fs = 0x23;
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
    frame->eip = load_vaddr;        /* program entry point */
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
    p->kernel_stack_top = kstack_top;
    p->load_vaddr = load_vaddr;
    p->load_pages = pages;
    p->stack_top = stack_top;
    p->state = PROC_READY;
    return p->pid;
}

/* Round robin: scans forward from just after `current`'s slot, wrapping,
 * returning the first non-zombie process found (which may be `current`
 * itself, if it's the only one left - callers that just marked `current`
 * a zombie rely on that state already being set before this runs, so it
 * can't be picked again). Returns 0 if every slot is a zombie. */
static process_t* pick_next_ready(void) {
    if (process_count == 0) return 0;

    int cur_idx = 0;
    for (int i = 0; i < process_count; i++) {
        if (&process_table[i] == current) {
            cur_idx = i;
            break;
        }
    }

    for (int off = 1; off <= process_count; off++) {
        int idx = (cur_idx + off) % process_count;
        if (process_table[idx].state != PROC_ZOMBIE) {
            return &process_table[idx];
        }
    }
    return 0;
}

void scheduler_tick(registers_t* regs) {
    if (!active || !current) return;

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
static void reap_all(void) {
    for (int i = 0; i < process_count; i++) {
        process_t* p = &process_table[i];
        for (uint32_t pg = 0; pg < p->load_pages; pg++) {
            paging_unmap_page(p->load_vaddr + pg * PAGE_SIZE);
        }
        paging_unmap_page(p->stack_top - PAGE_SIZE);
        kfree((void*)p->kernel_stack_base);
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

    current = next;
    current->state = PROC_RUNNING;
    gdt_set_kernel_stack(current->kernel_stack_top);
    scheduler_next_esp = current->esp;
    /* Returns normally: back up through the syscall handler -> isr
     * dispatch -> isr_common_stub, whose epilogue performs the actual
     * switch once it sees scheduler_next_esp set. */
}

void scheduler_run_until_idle(void) {
    if (process_count == 0) return;

    active = 1;
    tick_countdown = SCHED_TICKS_PER_SLICE;
    current = &process_table[0];
    current->state = PROC_RUNNING;
    gdt_set_kernel_stack(current->kernel_stack_top);

    /* Blocks here - from this function's own point of view - until every
     * spawned process has called sys_exit. See scheduler_asm.s. */
    scheduler_bootstrap_save_and_jump(current->esp);

    reap_all();
}
