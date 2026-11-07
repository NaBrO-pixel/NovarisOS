/* Fake "kernel" for host tests: implements the novac_sys_* hooks with mmap
 * so the allocator can be exercised, with fault injection. This is test
 * scaffolding, NOT a NovarisOS binding. */
#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <unistd.h>
#include <sys/mman.h>
#include "../../src/internal/sys.h"
#include "harness.h"

static int fail_count, map_calls, unmap_calls;
static size_t live;
static jmp_buf *exit_jmp;
static int exit_status;

void th_sys_fail_maps(int count) { fail_count = count; }
int th_sys_map_calls(void) { return map_calls; }
int th_sys_unmap_calls(void) { return unmap_calls; }
size_t th_sys_live_bytes(void) { return live; }

void *novac_sys_map(size_t *size)
{
    size_t granted = (*size + 4095u) & ~(size_t)4095u;
    void *p;
    map_calls++;
    if (fail_count != 0) { if (fail_count > 0) fail_count--; return NULL; }
    p = mmap(NULL, granted, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return NULL;
    memset(p, 0xA5, granted);       /* garbage, so calloc must really zero */
    live += granted;
    *size = granted;
    return p;
}

int novac_sys_unmap(void *addr, size_t size)
{
    unmap_calls++;
    live -= size;
    return munmap(addr, size);
}

_Noreturn void novac_sys_exit(int status)
{
    if (exit_jmp) { exit_status = status; longjmp(*exit_jmp, 1); }
    _exit(99);                       /* exit outside a guarded call: test bug */
}

int th_run_expect_exit(void (*fn)(void), int *status)
{
    jmp_buf jb;
    jmp_buf *prev = exit_jmp;
    int exited;
    exit_jmp = &jb;
    exited = setjmp(jb);
    if (!exited) fn();
    exit_jmp = prev;
    if (exited) *status = exit_status;
    return exited;
}
