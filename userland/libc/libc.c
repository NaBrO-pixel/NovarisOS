#include "libc.h"

#define SYS_EXIT  0
#define SYS_WRITE 1
#define SYS_MMAP  2

static inline int syscall1(int num, int arg1) {
    int ret;
    __asm__ __volatile__("int $0x80" : "=a"(ret) : "a"(num), "b"(arg1));
    return ret;
}

void write(const char* str) {
    syscall1(SYS_WRITE, (int)str);
}

void* mmap_anon(unsigned int size) {
    return (void*)syscall1(SYS_MMAP, (int)size);
}

void exit(int code) {
    syscall1(SYS_EXIT, code);
    for (;;) { } /* unreachable: sys_exit hands control back to the kernel */
}
