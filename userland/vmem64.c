/* vmem64.c - what a program is allowed to be told about its own address
 * space.
 *
 * Milestone 72. Every assertion here is one Wine makes on the way to
 * creating a prefix, reduced to the smallest program that makes it. They
 * were all answered wrongly by a kernel that passed every other test in
 * this tree, because each wrong answer was a plausible one:
 *
 *   - MAP_FIXED_NOREPLACE over occupied memory was granted rather than
 *     refused, and the pages already there were zeroed. Wine reserves
 *     0x7ffffe000000..0x7fffffff0000 PROT_NONE; that range contains the
 *     process stack, so the reservation quietly zeroed every saved
 *     return address Wine had. Nothing faulted at the time - execution
 *     ran on until the first `ret` popped a zero and jumped to 0.
 *
 *   - an mmap above the user half was granted. Wine measures the address
 *     space by mmapping one page at 1<<63 and halving until the kernel
 *     accepts it; a kernel that accepts the first try reports a limit of
 *     0xffffffffffff0000, and Wine sizes a table from it - a 32GB
 *     allocation where Linux asks for 2.2MB.
 *
 *   - mprotect returned success without doing anything, so a page made
 *     read-only could never be made writable again.
 *
 * The value of running this on Linux too is that none of the expected
 * answers below are this test's opinion. They are what Linux does.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <setjmp.h>
#include <signal.h>
#include <unistd.h>
#include <sys/mman.h>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

static int failures;

static void ok(const char *what, int cond)
{
    printf("%-4s %s\n", cond ? "ok" : "FAIL", what);
    if (!cond)
        failures++;
}

static int all_bytes_are(const unsigned char *p, unsigned char v, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (p[i] != v)
            return 0;
    return 1;
}

/* Wine's own probe, copied out of dlls/ntdll/unix/virtual.c so that what
 * is tested is the question Wine actually asks rather than a tidier one.
 * It mmaps a single page at 1<<63 and halves the address until the
 * kernel takes it, then calls twice the first address that worked the
 * end of the address space. */
static unsigned long host_addr_space_limit(void)
{
    unsigned long addr = 1UL << 63;

    while (addr >> 32) {
        void *ret = mmap((void *)addr, 4096, PROT_NONE,
                         MAP_PRIVATE | MAP_ANON | MAP_FIXED_NOREPLACE, -1, 0);
        if (ret != MAP_FAILED) {
            munmap(ret, 4096);
            if (ret >= (void *)addr)
                break;
        } else if (errno == EEXIST) {
            break;
        }
        addr >>= 1;
    }
    return (addr << 1) - 0x10000;
}

static sigjmp_buf jump;
static volatile sig_atomic_t caught;

static void on_segv(int sig)
{
    (void)sig;
    caught = 1;
    siglongjmp(jump, 1);
}

/* Writes one byte and reports whether that faulted rather than whether
 * it worked - the two are different questions once mprotect is real. */
static int write_faults(volatile unsigned char *p)
{
    caught = 0;
    if (sigsetjmp(jump, 1) == 0)
        *p = 0x5A;
    return caught;
}

#define SPAN (16 * 4096)

int main(void)
{
    unsigned char *area;
    void *got;
    struct sigaction sa;

    setvbuf(stdout, NULL, _IONBF, 0);

    /* --- MAP_FIXED_NOREPLACE must refuse, and must not touch --------- */

    area = mmap(NULL, SPAN, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANON, -1, 0);
    ok("a plain anonymous mapping", area != MAP_FAILED);
    memset(area, 0xA5, SPAN);

    /* Over the whole thing. */
    errno = 0;
    got = mmap(area, SPAN, PROT_NONE,
               MAP_PRIVATE | MAP_ANON | MAP_FIXED_NOREPLACE | MAP_NORESERVE,
               -1, 0);
    ok("MAP_FIXED_NOREPLACE over a mapped range is refused",
       got == MAP_FAILED);
    ok("and the refusal is EEXIST", errno == EEXIST);

    /* The assertion the stack corruption reduces to: a refused mapping
     * must leave the memory it was refused exactly as it was. */
    ok("and the range it was refused is untouched",
       all_bytes_are(area, 0xA5, SPAN));

    /* Overlapping by a single page at the end, which is the case a
     * kernel that probes only the first page of the range gets wrong -
     * and the case Wine's reserve_area recursion generates as it halves
     * a range down around an obstacle. */
    errno = 0;
    got = mmap(area - SPAN, SPAN + 4096, PROT_NONE,
               MAP_PRIVATE | MAP_ANON | MAP_FIXED_NOREPLACE | MAP_NORESERVE,
               -1, 0);
    if (got != MAP_FAILED)
        munmap(got, SPAN + 4096);
    ok("a range overlapping by one page is refused too", got == MAP_FAILED);
    ok("and that range is untouched as well",
       all_bytes_are(area, 0xA5, SPAN));

    /* And it must still succeed where the range really is free, or the
     * refusal above would be indistinguishable from refusing
     * everything. */
    munmap(area, SPAN);
    got = mmap(area, SPAN, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANON | MAP_FIXED_NOREPLACE, -1, 0);
    ok("MAP_FIXED_NOREPLACE over a free range succeeds", got == area);

    /* --- the end of the user half ------------------------------------ */

    errno = 0;
    got = mmap((void *)(1UL << 63), 4096, PROT_NONE,
               MAP_PRIVATE | MAP_ANON | MAP_FIXED_NOREPLACE, -1, 0);
    if (got != MAP_FAILED)
        munmap(got, 4096);
    ok("a fixed mapping at 1<<63 is refused", got == MAP_FAILED);

    printf("limit %lx\n", host_addr_space_limit());

    /* --- mprotect ----------------------------------------------------- */

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_segv;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);

    ok("a fresh read-write page takes a write", !write_faults(area));

    ok("mprotect to read-only reports success",
       mprotect(area, SPAN, PROT_READ) == 0);
    ok("and the page really is read-only now", write_faults(area));
    ok("and it is still readable", area[0] == 0x5A);

    ok("mprotect back to read-write reports success",
       mprotect(area, SPAN, PROT_READ | PROT_WRITE) == 0);
    ok("and the page really is writable again", !write_faults(area));

    munmap(area, SPAN);

    printf("vmem64: %d failures\n", failures);
    return failures ? 1 : 101;
}
