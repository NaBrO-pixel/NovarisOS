/* syscall64.c - the MSRs that make SYSCALL work, and what it dispatches to.
 *
 * This is the bring-up half of Milestone 44's item 4, and it is worth
 * being clear about what it is not. Novaris's 32-bit kernel implements
 * *Linux's i386 syscall ABI* - the numbers, the register convention and
 * the structure layouts - and that is precisely what lets real glibc and
 * real Wine run on it unmodified. None of that transfers to x86-64, which
 * has different numbers, a different register convention and different
 * structures. What is here is the mechanism: ring 3 is reachable, a
 * syscall arrives in C, and a value comes back. The ABI on top of it is
 * still to be written. */

#include "syscall64.h"
#include "gdt64.h"
#include "serial64.h"
#include "uspace64.h"
#include "win32_64.h"
#include "sched64.h"

#define IA32_EFER   0xC0000080u
#define IA32_STAR   0xC0000081u
#define IA32_LSTAR  0xC0000082u
#define IA32_FMASK  0xC0000084u

#define EFER_SCE    (1ULL << 0)

extern void syscall64_entry(void);

static uint64_t call_count;
static uint64_t last_arg;
static uint64_t exit_code;

static inline uint64_t read_msr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ __volatile__("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void write_msr(uint32_t msr, uint64_t value) {
    __asm__ __volatile__("wrmsr"
                         :: "c"(msr),
                            "a"((uint32_t)value),
                            "d"((uint32_t)(value >> 32)));
}

void syscall64_init(void) {
    uint64_t star;

    call_count = last_arg = exit_code = 0;

    /* Without SCE the instruction raises #UD rather than doing anything,
     * and the fault looks like a bad opcode rather than a missing bit. */
    write_msr(IA32_EFER, read_msr(IA32_EFER) | EFER_SCE);

    /* STAR[47:32] is the SYSCALL base, STAR[63:48] the SYSRET base. See
     * syscall64.h for how each expands into a CS/SS pair; the values are
     * 0x08 and 0x10 because of how gdt64.c orders its descriptors, and
     * changing that order silently changes which selectors land here. */
    star = ((uint64_t)GDT64_KCODE_SEL << 32)
         | ((uint64_t)GDT64_KDATA_SEL << 48);
    write_msr(IA32_STAR, star);

    write_msr(IA32_LSTAR, (uint64_t)&syscall64_entry);

    /* Bits set here are *cleared* in RFLAGS on entry. IF, so the handler
     * does not run with interrupts on while it is still sorting out its
     * stack; DF, so the string instructions in any C it calls count
     * upwards, which the SysV ABI requires and a ring-3 program is under
     * no obligation to leave true; TF, so a single-stepping debugger in
     * ring 3 does not trap inside the kernel. */
    write_msr(IA32_FMASK, (1ULL << 9) | (1ULL << 10) | (1ULL << 8));
}

static uint64_t bytes_written;

uint64_t syscall64_bytes_written(void) { return bytes_written; }

/* The number of the last call that had no implementation, and how many
 * there were. Discovering what a real program needs is done by running
 * it and reading these, since this kernel has no strace. */
static uint64_t unimpl_count, last_unimpl;

/* How many threads ended through exit(2) without ending the process. */
static uint64_t thread_exits;

uint64_t syscall64_thread_exits(void) { return thread_exits; }

uint64_t syscall64_unimplemented(void) { return last_unimpl; }
uint64_t syscall64_unimplemented_count(void) { return unimpl_count; }

/* Off by default: it prints on every call, so leaving it on would bury
 * the program's own output in the transcript the tests match against. */
static int trace;

void syscall64_set_trace(int on) { trace = on; }

static uint64_t dispatch(syscall64_args_t* args);

uint64_t syscall64_dispatch(syscall64_args_t* args) {
    uint64_t r;

    if (!trace) return dispatch(args);

    serial64_puts("NOVARIS64: [call] ");
    serial64_putdec(args->nr);
    serial64_puts("(");
    serial64_puthex(args->a1);
    serial64_puts(", ");
    serial64_puthex(args->a2);
    serial64_puts(", ");
    serial64_puthex(args->a3);
    serial64_puts(")");
    r = dispatch(args);
    serial64_puts(" = ");
    serial64_puthex(r);
    serial64_putc('\n');
    return r;
}

/* Called from syscall64_entry with a pointer to the pushed arguments. */
static uint64_t dispatch(syscall64_args_t* args) {
    uint64_t nr = args->nr;
    uint64_t a1 = args->a1, a2 = args->a2, a3 = args->a3;

    call_count++;

    /* A Win32 import, arriving through one of pe64.c's thunks. Checked
     * before the switch because the range is contiguous and has nothing
     * to do with Linux's numbering. */
    if (nr >= WIN32_64_BASE && nr <= WIN32_64_EXIT) {
        if (nr == WIN32_64_EXIT) {
            exit_code = a1;
            return a1;
        }
        return win32_64_call(nr, a1, a2, a3, args->a4);
    }

    switch (nr) {
    case SYS64_WRITE: {
        /* write(fd, buf, count). `buf` is a ring-3 pointer, and it is
         * dereferenced directly because the kernel is running in the
         * caller's address space - the high half is shared, so a syscall
         * never had to leave the space it was called from.
         *
         * It is also completely unchecked, which is the honest state of
         * this ABI: a real implementation validates that the range is
         * mapped and belongs to the caller before touching it, and does
         * so without racing the caller. Nothing here does that yet. */
        const char* buf = (const char*)a2;
        uint64_t n = a3;
        if (a1 != 1 && a1 != 2) return (uint64_t)-1;   /* stdout/stderr */
        for (uint64_t i = 0; i < n; i++) serial64_putc(buf[i]);
        bytes_written += n;
        return n;
    }
    /* writev(fd, iov, iovcnt) - glibc's stdio uses this rather than
     * write() as soon as it has a buffer to flush. */
    case SYS64_WRITEV: {
        const struct { const char* base; uint64_t len; }* iov =
            (const void*)a2;
        uint64_t total = 0;
        if (a1 != 1 && a1 != 2) return (uint64_t)-9;   /* -EBADF */
        for (uint64_t i = 0; i < a3; i++) {
            for (uint64_t j = 0; j < iov[i].len; j++)
                serial64_putc(iov[i].base[j]);
            total += iov[i].len;
        }
        bytes_written += total;
        return total;
    }

    case SYS64_BRK:
        return uspace64_brk(a1);

    case SYS64_MMAP:
        /* Only anonymous mappings. A file mapping would need a
         * filesystem, which the 64-bit tree does not have. */
        if (!(args->a4 & 0x20)) return (uint64_t)-19;  /* MAP_ANONYMOUS */
        return uspace64_mmap(a1, a2, a3, args->a4);

    case SYS64_MUNMAP:
        return uspace64_munmap(a1, a2);

    case SYS64_MPROTECT:
        /* Accepted and ignored. Every mapping this kernel makes is
         * already readable and writable by the process that owns it, so
         * the only thing an honest implementation would add here is
         * *removing* permissions - and nothing yet depends on that. */
        return 0;

    /* arch_prctl(ARCH_SET_FS, addr) is how a thread pointer is set on
     * x86-64, and glibc does it before it can touch a single piece of
     * thread-local storage - errno included. Nothing works before this. */
    case SYS64_ARCH_PRCTL:
        switch (a1) {
        case 0x1002:                                   /* ARCH_SET_FS */
            write_msr(0xC0000100u, a2);
            return 0;
        case 0x1001:                                   /* ARCH_SET_GS */
            write_msr(0xC0000101u, a2);
            return 0;
        default:
            return (uint64_t)-22;                      /* -EINVAL */
        }

    /* clone(flags, child_stack, parent_tid, child_tid, tls)
     *
     * Threads only - the CLONE_VM|CLONE_THREAD case. A fork, which is
     * the same syscall without CLONE_VM, would have to copy the address
     * space; nothing here does that, so it is refused rather than
     * silently producing a second thread where a child process was
     * asked for.
     *
     * The child is its parent with three differences, which is exactly
     * what Linux specifies: a different stack, a thread pointer of its
     * own, and rax = 0 so the two can tell each other apart on return.
     * Everything else is inherited, which is why the entry stub saves
     * the callee-saved registers it is otherwise entitled to ignore. */
    case SYS64_CLONE: {
        registers64_t child;
        const vmspace64_t* space;
        int tid;

        if (!(a1 & CLONE_VM))    return (uint64_t)-38;  /* -ENOSYS: fork */
        if (!a2)                 return (uint64_t)-22;  /* -EINVAL       */

        space = sched64_current_space();
        if (!space) return (uint64_t)-38;

        child.rax = 0;            /* how the child knows it is the child */
        child.rbx = args->rbx;
        child.rcx = 0;            /* SYSCALL destroyed it in the parent too */
        child.rdx = a3;
        child.rsi = a2;
        child.rdi = a1;
        child.rbp = args->rbp;
        child.r8  = args->a5;
        child.r9  = args->a6;
        child.r10 = args->a4;
        child.r11 = 0;
        child.r12 = args->r12;
        child.r13 = args->r13;
        child.r14 = args->r14;
        child.r15 = args->r15;

        child.int_no   = 0;
        child.err_code = 0;
        child.rip      = args->ret_rip;      /* just after its `syscall` */
        child.cs       = 0x23;
        child.rflags   = args->ret_rflags | 0x200;   /* IF, or it never
                                                      * yields the CPU  */
        child.rsp      = a2;
        child.ss       = 0x1B;

        tid = sched64_add_frame(&child, space,
                                (a1 & CLONE_SETTLS) ? args->a5 : 0);
        if (tid < 0) return (uint64_t)-11;              /* -EAGAIN */

        /* The parent gets the child's tid; the child gets 0 above. */
        return (uint64_t)tid + 1;
    }

    case SYS64_GETTID:
        return (uint64_t)sched64_current() + 1;

    case SYS64_SET_TID_ADDRESS:
        return (uint64_t)sched64_current() + 1;

    case SYS64_SET_ROBUST_LIST:
        return 0;

    case SYS64_IOCTL:
        /* glibc asks whether fd 1 is a terminal to choose line
         * buffering. -ENOTTY makes it a fully buffered stream, which is
         * correct here: this is a serial port, not a tty. */
        return (uint64_t)-25;

    case SYS64_FSTAT: {
        /* Only the fields glibc's stdio reads: it wants st_mode to
         * decide the stream type and st_blksize to size the buffer. */
        uint8_t* st = (uint8_t*)a2;
        for (int i = 0; i < 144; i++) st[i] = 0;
        *(uint32_t*)(st + 24) = 0020620u;  /* st_mode: S_IFCHR | 0620 */
        *(uint64_t*)(st + 56) = 1024;      /* st_blksize                */
        return 0;
    }

    case SYS64_GETRANDOM: {
        /* Not random. It is deterministic and it is documented as such,
         * because a kernel with no entropy source that pretends
         * otherwise is worse than one that says so. glibc uses this for
         * the stack guard, which this makes predictable. */
        uint8_t* buf = (uint8_t*)a1;
        static uint64_t seed = 0x2545F4914F6CDD1DULL;
        for (uint64_t i = 0; i < a2; i++) {
            seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
            buf[i] = (uint8_t)seed;
        }
        return a2;
    }

    case SYS64_RT_SIGACTION:
    case SYS64_RT_SIGPROCMASK:
        /* Accepted so that startup proceeds; no signal is ever
         * delivered, so agreeing to a handler costs nothing and lying
         * about it costs nothing yet either. */
        return 0;

    case SYS64_READ:
        return 0;                                      /* end of file */

    case SYS64_ECHO:
        last_arg = a1;
        return a1 + 0x1111;
    /* exit(2) ends the calling THREAD. exit_group(2) ends the process.
     *
     * Treating them as the same thing is a real divergence and it was
     * observable: userland/thread64.s, run on Linux with exit(60), hung
     * - the parent thread ended and the parked child kept the process
     * alive. Novaris passed that same program, because it had only one
     * behaviour for both. */
    case SYS64_EXIT: {
        registers64_t next;
        vmspace64_t next_space;
        uint64_t next_fs;

        thread_exits++;
        if (sched64_exit_current(&next, &next_space, &next_fs)) {
            /* A sibling is still runnable, so this thread simply stops
             * existing and that one continues. There is no returning to
             * the caller: the thread it would return to is gone. */
            vmspace64_switch(&next_space);
            write_msr(0xC0000100u, next_fs);        /* its own TLS */
            sched64_resume(&next);                  /* never returns */
        }
        /* The last thread. Falling through leaves ring 3 the way
         * exit_group does, which is correct - the process is over. */
        exit_code = a1;
        return a1;
    }

    case SYS64_EXIT_GROUP:
        exit_code = a1;
        return a1;
    default:
        /* Said out loud, because there is no strace here and the only
         * way to find out what a real program wants is to let it ask. */
        unimpl_count++;
        last_unimpl = nr;
        serial64_puts("NOVARIS64: [enosys] syscall ");
        serial64_putdec(nr);
        serial64_putc('\n');
        /* Linux answers an unimplemented call with -ENOSYS, and programs
         * do check for it, so this is -38 rather than -1. */
        return (uint64_t)-38;
    }
}

uint64_t syscall64_count(void)     { return call_count; }
uint64_t syscall64_last_arg(void)  { return last_arg; }
uint64_t syscall64_exit_code(void) { return exit_code; }
