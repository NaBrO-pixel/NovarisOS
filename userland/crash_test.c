/* crash_test.c - an ELF program that faults on purpose.
 *
 * The POSIX counterpart of userland/pe_test/crash.c: a bad program must
 * take only itself down, and the shell must come back. Written for Linux
 * like the other tests here (raw int $0x80, no libc), though unlike them
 * it is not transcript-compared - on Linux it dies with SIGSEGV and the
 * shell prints its own message, which is a different transcript for the
 * same correct behaviour.
 *
 * It exists because of a real bug. Milestone 20 moved ELF programs onto
 * the scheduler but left the fault path unwinding through
 * process_run_user_mode()'s saved stack, so a faulting ELF program
 * panicked the kernel instead of returning to the shell. Nothing caught
 * it: every ELF test program until now ran to completion.
 */

static long sc1(long n, long a) {
    long r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "a"(n), "b"(a) : "memory");
    return r;
}
static long sc3(long n, long a, long b, long c) {
    long r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "a"(n), "b"(a), "c"(b), "d"(c)
                         : "memory");
    return r;
}
static unsigned slen(const char* s) { unsigned n = 0; while (s[n]) n++; return n; }
static void out(const char* s) { sc3(4, 1, (long)s, slen(s)); }

void _start(void) {
    out("about to dereference a null pointer on purpose\n");
    out("Novaris should report the fault and return to the shell\n");

    volatile int* p = (volatile int*)0;
    *p = 1;

    out("UNREACHABLE: the store above should have faulted\n");
    sc1(1, 1);
    __builtin_unreachable();
}
