#include "libc.h"

/* Linux/i386 syscall numbers - Milestone 18 replaced Novaris's ad-hoc
 * three with the real ABI. See include/posix.h in the kernel for why. */
#define SYS_exit   1
#define SYS_write  4
#define SYS_mmap2 192

#define MAP_PRIVATE   0x02
#define MAP_ANONYMOUS 0x20
#define PROT_READ     0x1
#define PROT_WRITE    0x2

static inline int syscall1(int num, int arg1) {
    int ret;
    __asm__ __volatile__("int $0x80" : "=a"(ret) : "a"(num), "b"(arg1));
    return ret;
}

static inline int syscall3(int num, int a1, int a2, int a3) {
    int ret;
    __asm__ __volatile__("int $0x80"
                         : "=a"(ret)
                         : "a"(num), "b"(a1), "c"(a2), "d"(a3));
    return ret;
}

static inline int syscall6(int num, int a1, int a2, int a3,
                           int a4, int a5, int a6) {
    int ret;
    /* ebx and ebp both need saving: ebx is the PIC register and ebp is the
     * frame pointer, and the sixth argument goes in ebp. */
    __asm__ __volatile__("push %%ebp\n\t"
                         "mov %7, %%ebp\n\t"
                         "int $0x80\n\t"
                         "pop %%ebp"
                         : "=a"(ret)
                         : "a"(num), "b"(a1), "c"(a2), "d"(a3),
                           "S"(a4), "D"(a5), "g"(a6)
                         : "memory");
    return ret;
}

static unsigned int slen(const char* s) {
    unsigned int n = 0;
    while (s[n]) n++;
    return n;
}

void write(const char* str) {
    /* Linux's write() is counted bytes on a file descriptor, not a
     * NUL-terminated string on an implicit console. */
    syscall3(SYS_write, 1, (int)str, (int)slen(str));
}

void* mmap_anon(unsigned int size) {
    int r = syscall6(SYS_mmap2, 0, (int)size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    /* Errors come back as -errno in [-4095, -1], the Linux convention. */
    if (r < 0 && r > -4096) return 0;
    return (void*)r;
}

void exit(int code) {
    syscall1(SYS_exit, code);
    for (;;) { } /* unreachable: exit hands control back to the kernel */
}
