/* thread_posix_test.c - Linux threads, the Unix way, run on Novaris.
 *
 * Same rules as posix_test.c and signal_test.c: raw `int $0x80`, Linux
 * syscall numbers, `gcc -m32 -static -nostdlib -ffreestanding`, linked
 * against nothing. No pthreads, because there is no libc here - this is
 * what pthreads is *made of*: clone() to create a thread, a user_desc to
 * give it thread-local storage behind gs, a futex to make it wait, and
 * CLONE_CHILD_CLEARTID to let a joiner find out it has finished.
 *
 * The same binary is run on the Linux build host and on Novaris and the
 * transcripts compared.
 *
 * See ROADMAP.md Milestone 20.
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
static long sc6(long n, long a, long b, long c, long d, long e, long f) {
    long r;
    __asm__ __volatile__("push %%ebp\n\t" "mov %7, %%ebp\n\t"
                         "int $0x80\n\t" "pop %%ebp"
                         : "=a"(r)
                         : "a"(n), "b"(a), "c"(b), "d"(c), "S"(d), "D"(e), "g"(f)
                         : "memory");
    return r;
}

#define SYS_exit            1
#define SYS_write           4
#define SYS_clone         120
#define SYS_mmap2         192
#define SYS_futex         240
#define SYS_set_thread_area 243
#define SYS_gettid        224
#define SYS_time           13

#define CLONE_VM             0x00000100
#define CLONE_FS             0x00000200
#define CLONE_FILES          0x00000400
#define CLONE_SIGHAND        0x00000800
#define CLONE_THREAD         0x00010000
#define CLONE_SETTLS         0x00080000
#define CLONE_PARENT_SETTID  0x00100000
#define CLONE_CHILD_CLEARTID 0x00200000

#define FUTEX_WAIT 0
#define FUTEX_WAKE 1

#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define MAP_PRIVATE   0x02
#define MAP_ANONYMOUS 0x20

/* i386's struct user_desc - what set_thread_area takes and CLONE_SETTLS
 * points at. */
struct user_desc {
    unsigned int entry_number;
    unsigned int base_addr;
    unsigned int limit;
    unsigned int flags;
};
/* seg_32bit (bit 0) | useable (bit 6): a 32-bit, byte-granular,
 * read/write data segment. */
#define UD_FLAGS 0x41

/* Which GDT entry the kernel gave us. It has to be *asked for* rather
 * than assumed, and then reused for every thread, because:
 *
 *   - the number differs by kernel. i386 Linux uses entries 6-8; a 32-bit
 *     process on an x86-64 kernel gets 12-14; Novaris has one, at 7.
 *   - CLONE_SETTLS cannot allocate. set_thread_area(-1) can, and reports
 *     back what it chose, so the portable sequence is "allocate once,
 *     then name that entry in every clone" - which is exactly what glibc
 *     does, and the reason this program runs on all three. */
static unsigned tls_entry = 0xFFFFFFFFu;

/* --- a few bytes of libc ------------------------------------------------- */

static unsigned slen(const char* s) { unsigned n = 0; while (s[n]) n++; return n; }
static void out(const char* s) { sc3(SYS_write, 1, (long)s, slen(s)); }
static void out_dec(long v) {
    char buf[24]; int i = 23; int neg = v < 0;
    unsigned long u = neg ? (unsigned long)(-v) : (unsigned long)v;
    buf[i--] = '\0';
    if (!u) buf[i--] = '0';
    while (u) { buf[i--] = (char)('0' + (u % 10)); u /= 10; }
    if (neg) buf[i--] = '-';
    out(&buf[i + 1]);
}
static void check(const char* label, int ok) {
    out(ok ? "  [ok]   " : "  [FAIL] ");
    out(label);
    out("\n");
}

/* --- TLS ----------------------------------------------------------------- */

/* Each thread's TLS block starts with its own tag, so reading gs:[0] is a
 * direct test of whether the segment really is per-thread. */
static unsigned tls_tag(void) {
    unsigned v;
    __asm__ __volatile__("movl %%gs:0, %0" : "=r"(v));
    return v;
}

static int tls_install(struct user_desc* ud, void* block) {
    ud->entry_number = 0xFFFFFFFFu;   /* "allocate one for me" */
    ud->base_addr = (unsigned)block;
    ud->limit = 0xFFF;
    ud->flags = UD_FLAGS;
    long r = sc1(SYS_set_thread_area, (long)ud);
    if (r != 0) return 0;
    tls_entry = ud->entry_number;
    /* The selector is the entry number the kernel chose, shifted, with
     * RPL 3 - computed rather than hardcoded, so this works wherever the
     * kernel happens to put it. */
    unsigned sel = (ud->entry_number << 3) | 3;
    __asm__ __volatile__("movw %w0, %%gs" : : "r"(sel));
    return 1;
}

/* --- a futex mutex ------------------------------------------------------- */

static int mutex = 0;

static void lock(int* m) {
    while (__sync_lock_test_and_set(m, 1)) {
        /* Held. Sleep until the holder wakes us - and pass the value we
         * expect, so a release between the test above and this call is
         * noticed rather than slept through. */
        sc6(SYS_futex, (long)m, FUTEX_WAIT, 1, 0, 0, 0);
    }
}

static void unlock(int* m) {
    __sync_lock_release(m);
    sc6(SYS_futex, (long)m, FUTEX_WAKE, 1, 0, 0, 0);
}

/* --- the workers --------------------------------------------------------- */

#define WORKERS    3
#define PER_THREAD 20

static volatile long guarded = 0;
static volatile int  tls_seen[WORKERS + 1];
static volatile int  ran[WORKERS + 1];

static int worker(void* arg) {
    int id = (int)(long)arg;
    tls_seen[id] = (int)tls_tag();
    ran[id] = 1;

    for (int i = 0; i < PER_THREAD; i++) {
        /* Unguarded ring-3 work, so the timer has somewhere to preempt
         * this thread that is not inside a syscall. */
        for (volatile int s = 0; s < 30000; s++) { }

        lock(&mutex);
        long seen = guarded;
        /* Held across a long stretch of ring-3 work, on purpose: the lock
         * is only being *tested* if a preemption lands between the read
         * and the write above and below it. 20000 was not enough on
         * Novaris, whose time slice is 20ms - every worker finished
         * inside one, and the counter came out right without the lock
         * doing anything at all. */
        for (volatile int s = 0; s < 300000; s++) { }
        guarded = seen + 1;
        unlock(&mutex);
    }
    return 0;
}

/* clone() with a child that runs `fn(arg)` on its own stack and exits.
 * The child cannot simply return - there is nothing to return to - so the
 * asm calls the function and then issues exit(). */
/* --- Milestone 25: a thread whose whole job is to be blocked ------------
 *
 * `wait_word` is never changed, so this thread cannot come out of the
 * wait by re-testing it. Only a FUTEX_WAKE can end it, which is what
 * makes the return value it records meaningful. */
static volatile int wait_word = 0;
static volatile int never_woken = 0;
static volatile int waiter_ready = 0, waiter_done = 0;
static volatile long waiter_rc = 12345;

static int waiter(void* arg) {
    (void)arg;
    waiter_ready = 1;
    waiter_rc = sc6(SYS_futex, (long)&wait_word, FUTEX_WAIT, 0, 0, 0, 0);
    waiter_done = 1;
    return 0;
}

static int spawn(int (*fn)(void*), void* arg, void* stack_top,
                 struct user_desc* tls, int* ctid) {
    unsigned long* sp = (unsigned long*)stack_top;
    *--sp = (unsigned long)arg;
    *--sp = (unsigned long)fn;

    long flags = CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND |
                 CLONE_THREAD | CLONE_SETTLS | CLONE_CHILD_CLEARTID;
    int ret;
    __asm__ __volatile__(
        "int $0x80\n\t"
        "testl %%eax, %%eax\n\t"
        "jnz 1f\n\t"
        /* Child. esp is the stack handed in, with fn and arg on top. */
        "popl %%eax\n\t"
        "popl %%ebx\n\t"
        "pushl %%ebx\n\t"
        "calll *%%eax\n\t"
        "movl $1, %%eax\n\t"      /* SYS_exit - ends this thread only */
        "xorl %%ebx, %%ebx\n\t"
        "int $0x80\n\t"
        "hlt\n\t"
        "1:\n\t"
        : "=a"(ret)
        : "0"(SYS_clone), "b"(flags), "c"(sp), "d"(0), "S"(tls), "D"(ctid)
        : "memory");
    return ret;
}

int main_(void) {
    out("posix thread test - clone, futex and TLS, running unmodified\n\n");

    /* 1. TLS for the main thread. */
    out("1. set_thread_area\n");
    static unsigned main_block[16];
    struct user_desc main_ud;
    main_block[0] = 1000;
    check("set_thread_area accepted", tls_install(&main_ud, main_block));
    check("gs:0 reads the main thread's own block", tls_tag() == 1000);

    /* 2. clone three threads, each with its own stack and TLS. */
    out("\n2. clone\n");
    static struct user_desc uds[WORKERS];
    static unsigned* blocks[WORKERS];
    static int tids[WORKERS];
    int spawned = 0;

    for (int i = 0; i < WORKERS; i++) {
        long stack = sc6(SYS_mmap2, 0, 65536, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        long blk = sc6(SYS_mmap2, 0, 4096, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if ((stack < 0 && stack > -4096) || (blk < 0 && blk > -4096)) break;

        blocks[i] = (unsigned*)blk;
        blocks[i][0] = (unsigned)(2000 + i);   /* this thread's TLS tag */

        uds[i].entry_number = tls_entry;   /* not -1: see tls_entry */
        uds[i].base_addr = (unsigned)blk;
        uds[i].limit = 0xFFF;
        uds[i].flags = UD_FLAGS;

        int r = spawn(worker, (void*)(long)(i + 1),
                      (void*)(stack + 65536), &uds[i], &tids[i]);
        if (r <= 0) break;
        tids[i] = r;
        spawned++;
    }
    out("  spawned ");
    out_dec(spawned);
    out(" threads\n");
    check("all three clones succeeded", spawned == WORKERS);

    /* 3. Join, the way pthread_join does it: wait on the tid word the
     *    kernel was told to clear when the thread exits. */
    out("\n3. join via CLONE_CHILD_CLEARTID + futex\n");
    for (int i = 0; i < spawned; i++) {
        int v;
        while ((v = tids[i]) != 0) {
            sc6(SYS_futex, (long)&tids[i], FUTEX_WAIT, v, 0, 0, 0);
        }
    }
    check("every thread's tid word was cleared on exit", 1);

    int all_ran = 1;
    for (int i = 1; i <= spawned; i++) if (!ran[i]) all_ran = 0;
    check("every worker actually ran", all_ran);

    /* 4. Each thread saw its own TLS block, not another's. */
    out("\n4. per-thread TLS\n");
    int distinct = 1;
    for (int i = 1; i <= spawned; i++) {
        if (tls_seen[i] != 2000 + (i - 1)) distinct = 0;
    }
    for (int i = 1; i <= spawned; i++) {
        out("  worker ");
        out_dec(i);
        out(" read gs:0 = ");
        out_dec(tls_seen[i]);
        out("\n");
    }
    check("each worker read its own TLS tag", distinct);
    check("the main thread's TLS survived", tls_tag() == 1000);

    /* 5. The futex mutex actually serialised the counter. */
    out("\n5. futex mutex\n");
    long expected = (long)spawned * PER_THREAD;
    out("  guarded counter = ");
    out_dec(guarded);
    out(", expected ");
    out_dec(expected);
    out("\n");
    check("the futex mutex protected the counter", guarded == expected);

    /* 6. Milestone 25: what a *blocking* futex returns, as opposed to one
     *    that spins until the word changes. All three of these were
     *    different on Novaris than on Linux until it really blocked, and
     *    nothing in this test had ever looked at a return value. */
    out("\n6. futex return values\n");
    {
        long stack = sc6(SYS_mmap2, 0, 65536, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        long blk = sc6(SYS_mmap2, 0, 4096, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        int have = !(stack < 0 && stack > -4096) && stack != 0 &&
                   !(blk < 0 && blk > -4096) && blk != 0;
        check("mapped a stack for the waiter", have);

        if (have) {
            /* CLONE_SETTLS is not optional here - the clone above sets it
             * unconditionally, and a null user_desc is -EFAULT. */
            static struct user_desc wud;
            wud.entry_number = tls_entry;
            wud.base_addr = (unsigned)blk;
            wud.limit = 0xFFF;
            wud.flags = UD_FLAGS;

            int tid = 0;
            long r = spawn(waiter, 0, (void*)(stack + 65536), &wud, &tid);
            check("the waiter thread started", r > 0);

            /* Wait until it is about to make the call, then give it long
             * enough to actually get there. A 20ms slice swallows about
             * 20 000 spin iterations, so this is several slices' worth -
             * the waiter only has two instructions to execute after
             * setting the flag. */
            while (!waiter_ready) { }
            for (volatile long i = 0; i < 400000; i++) { }

            /* The word is never changed, so the waiter cannot fall out of
             * the wait on its own: only this wake can end it. Looping
             * bounds a lost race rather than papering over the result -
             * the assertions below are on the values, not on the loop. */
            long woke = 0;
            for (int t = 0; t < 200 && woke == 0; t++) {
                woke = sc6(SYS_futex, (long)&wait_word, FUTEX_WAKE, 1, 0, 0, 0);
                if (woke == 0) for (volatile long i = 0; i < 100000; i++) { }
            }
            while (!waiter_done) { }

            check("FUTEX_WAKE reports how many it woke", woke == 1);
            check("FUTEX_WAIT returns 0 when it is woken", waiter_rc == 0);
            check("a wake with nobody waiting reports 0",
                  sc6(SYS_futex, (long)&wait_word, FUTEX_WAKE, 1, 0, 0, 0) == 0);
            while (tid != 0) {
                sc6(SYS_futex, (long)&tid, FUTEX_WAIT, tid, 0, 0, 0);
            }
        }
    }

    /* 7. And a timed wait, which used to be an untimed one: the fourth
     *    argument was ignored, so a bounded wait became unbounded. The
     *    elapsed check matters - returning -ETIMEDOUT immediately would
     *    satisfy the return value and none of the meaning. */
    out("\n7. futex with a timeout\n");
    {
        struct { long tv_sec; long tv_nsec; } ts;
        ts.tv_sec = 1;
        ts.tv_nsec = 500000000;   /* 1.5s, over time()'s one-second grain */

        long t0 = sc1(SYS_time, 0);
        long rc = sc6(SYS_futex, (long)&never_woken, FUTEX_WAIT, 0,
                      (long)&ts, 0, 0);
        long t1 = sc1(SYS_time, 0);

        check("a wait nobody satisfies returns -ETIMEDOUT", rc == -110);
        check("and it really waited", (t1 - t0) >= 1);
    }

    out("\nposix thread test done.\n");
    return 0;
}

void _start(void) {
    sc1(SYS_exit, main_());
    __builtin_unreachable();
}
