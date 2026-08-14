/* hello_glibc64.c - an ordinary C program, linked against real glibc.
 *
 * The difference between this and userland/hello64.s is the whole point.
 * That one issues three syscalls by hand; this one goes through glibc's
 * startup - which sets up thread-local storage, reads its own program
 * headers out of the auxiliary vector, initialises malloc, builds a
 * buffered stdio stream and only then reaches main. Every one of those
 * steps is a demand on the kernel's ABI, and none of them are optional
 * or negotiable: glibc was compiled long before Novaris existed.
 *
 * It is the same reason the 32-bit tree's milestones lean on real glibc
 * rather than a hand-rolled libc. A kernel that runs this is a kernel
 * that implements Linux, not something Linux-shaped.
 *
 * printf rather than puts, so that stdio's buffering and its vararg
 * formatting are both exercised; malloc so that brk or mmap has to work.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
    char *buf = malloc(128);
    int i, sum = 0;

    if (!buf)
        return 1;

    strcpy(buf, "hello from glibc on Novaris");

    for (i = 1; i <= 10; i++)
        sum += i;

    /* Deliberately not printing the pointer itself. The whole output has
     * to be byte-identical to the host's for the differential in
     * Makefile.amd64 to compare it, and an address is the one thing that
     * legitimately differs between two kernels. */
    printf("%s\n", buf);
    printf("malloc %s, 1..10 sums to %d\n", buf ? "ok" : "failed", sum);

    free(buf);

    /* A distinctive status, so the kernel can tell a real exit from
     * anything else that might end the run. */
    return 7;
}
