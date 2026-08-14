/* sched64.c - round-robin preemption, done inside the timer interrupt. */

#include "sched64.h"
#include "gdt64.h"

#define UCODE_SEL_RPL3 0x23
#define UDATA_SEL_RPL3 0x1B
#define RFLAGS_IF      0x202    /* IF, plus the bit that is always set */

typedef struct {
    registers64_t regs;
    vmspace64_t   space;
    int           used;
} task64_t;

static task64_t tasks[SCHED64_MAX_TASKS];
static int      task_total;
static int      current = -1;
static uint64_t switch_count;
static uint64_t stop_after;
static uint64_t stop_rip;

void sched64_init(void) {
    for (int i = 0; i < SCHED64_MAX_TASKS; i++) tasks[i].used = 0;
    task_total = 0;
    current = -1;
    switch_count = 0;
    stop_after = 0;
    stop_rip = 0;
}

int sched64_add(uint64_t rip, uint64_t rsp, uint64_t arg,
                const vmspace64_t* space) {
    int i;

    for (i = 0; i < SCHED64_MAX_TASKS; i++) if (!tasks[i].used) break;
    if (i == SCHED64_MAX_TASKS) return -1;

    for (uint64_t* p = (uint64_t*)&tasks[i].regs;
         p < (uint64_t*)(&tasks[i].regs + 1); p++) *p = 0;

    /* The frame iretq will consume the first time this task is resumed.
     * cs and ss are the ring-3 selectors; without IF in rflags the task
     * would take the CPU and never give it back, since the only thing
     * that preempts it is the timer. */
    tasks[i].regs.rip    = rip;
    tasks[i].regs.rsp    = rsp;
    tasks[i].regs.cs     = UCODE_SEL_RPL3;
    tasks[i].regs.ss     = UDATA_SEL_RPL3;
    tasks[i].regs.rflags = RFLAGS_IF;
    tasks[i].regs.rdi    = arg;

    tasks[i].space = *space;
    tasks[i].used  = 1;
    task_total++;
    return i;
}

void sched64_set_current(int index) { current = index; }
int  sched64_current(void)          { return current; }
uint64_t sched64_switches(void)     { return switch_count; }

void sched64_stop_after(uint64_t switches, uint64_t exit_rip) {
    stop_after = switches;
    stop_rip   = exit_rip;
}

static int next_task(int from) {
    for (int n = 1; n <= SCHED64_MAX_TASKS; n++) {
        int i = (from + n) % SCHED64_MAX_TASKS;
        if (tasks[i].used) return i;
    }
    return from;
}

void sched64_tick(registers64_t* frame) {
    int next;

    if (current < 0 || task_total < 2) return;

    next = next_task(current);
    if (next == current) return;

    /* Everything the outgoing task is, as the CPU and the stub left it. */
    tasks[current].regs = *frame;

    current = next;

    /* And the incoming task, written over the same frame - iretq reloads
     * from exactly this memory when the stub returns. */
    *frame = tasks[current].regs;
    vmspace64_switch(&tasks[current].space);
    switch_count++;

    /* Ending the run by redirecting a resume rather than by counting
     * iterations inside the task: every other register is restored
     * untouched, so the task arrives at the exit stub with the pointer
     * it was using still in place. */
    if (stop_after && switch_count >= stop_after && stop_rip)
        frame->rip = stop_rip;
}
