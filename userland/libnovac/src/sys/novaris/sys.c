/* NovarisOS binding for the three kernel hooks in src/internal/sys.h.
 * Built only for the real target (Makefile NOVAC_SYS_SRC), never for host
 * unit tests. Uses the Linux x86-64 calls NovarisOS implements:
 * mmap(9), munmap(11), exit_group(231). */
#include <stddef.h>
#include "../../internal/sys.h"
#include "../../internal/syscall.h"

#define PROT_READ     0x1
#define PROT_WRITE    0x2
#define MAP_PRIVATE   0x02
#define MAP_ANONYMOUS 0x20
#define PAGE_SIZE     4096u

void *novac_sys_map(size_t *size)
{
    size_t want = *size, granted;
    long r;
    if (want == 0 || want > (size_t)-1 - (PAGE_SIZE - 1)) return NULL;
    granted = (want + PAGE_SIZE - 1) & ~(size_t)(PAGE_SIZE - 1);
    r = novac_syscall6(NOVAC_SYS_mmap, 0, (long)granted, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (r < 0 && r >= -NOVAC_MAX_ERRNO) return NULL;
    *size = granted;
    return (void *)r;
}

int novac_sys_unmap(void *addr, size_t size)
{
    return novac_syscall6(NOVAC_SYS_munmap, (long)addr, (long)size, 0, 0, 0, 0) == 0 ? 0 : -1;
}

_Noreturn void novac_sys_exit(int status)
{
    for (;;) novac_syscall6(NOVAC_SYS_exit_group, status, 0, 0, 0, 0, 0);
}
