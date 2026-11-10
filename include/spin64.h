#ifndef SPIN64_H
#define SPIN64_H

#include <stdint.h>
#include "gdt64.h"

/* spin64 - the small locks under the kernel lock (Milestone 102).
 *
 * The kernel lock (smp64.c) still covers most of the kernel. These cover
 * what is now also reached without it: the pipes (pipe64.c), the task
 * table's wake and block state (sched64.c) and the frame allocator
 * (pmm64.c). The order is fixed - the kernel lock, then a pipe, then the
 * task table, then the allocator - and nothing is called with one of
 * these held that could take one earlier in that list.
 *
 * Interrupts are off while one is held: kernel_main runs with them on,
 * and the timer's handler reaches the task table. Recursive per CPU, so
 * a function that takes one may call another that takes it too. */

typedef struct {
    volatile int lock;
    volatile int owner;
    int          depth;
} spin64_t;

#define SPIN64_INIT { 0, -1, 0 }

/* How many of these each CPU holds (smp64.c): a CPU holding one must not
 * start a TLB shootdown, since another may be spinning for it with
 * interrupts off and never answer. */
extern volatile int spin64_held[];

static inline uint64_t spin64_acquire(spin64_t* l) {
    uint64_t fl;
    int me;
    __asm__ __volatile__("pushfq; pop %0; cli" : "=r"(fl) :: "memory");
    me = gdt64_cpu();
    if (l->owner == me) { l->depth++; return fl; }
    while (__atomic_exchange_n(&l->lock, 1, __ATOMIC_ACQUIRE))
        while (l->lock) __asm__ __volatile__("pause");
    l->owner = me;
    l->depth = 1;
    spin64_held[me & 7]++;
    return fl;
}

static inline void spin64_release(spin64_t* l, uint64_t fl) {
    if (--l->depth == 0) {
        spin64_held[l->owner & 7]--;
        l->owner = -1;
        __atomic_store_n(&l->lock, 0, __ATOMIC_RELEASE);
    }
    if (fl & 0x200) __asm__ __volatile__("sti" ::: "memory");
}

#endif
