/* mmap_test.c - a Linux program about shared memory and reservations,
 * run unmodified on Novaris.
 *
 * Same rules as the rest of the comparison binaries: raw `int $0x80`,
 * Linux syscall numbers, `gcc -m32 -static -nostdlib -ffreestanding`,
 * linked against nothing.
 *
 * What it is for. Milestone 29 got real Wine as far as starting a
 * wineserver and stopped at one syscall flag: MAP_SHARED. Two processes
 * mapping one file have to see the same memory, because that is how
 * wineserver publishes the Windows shared-data page to every client.
 * Chasing that turned up three more pieces of mmap that Novaris had
 * approximated:
 *
 *   - PROT_NONE was a *mapping* rather than a reservation, so reserving
 *     address space cost a physical page per page reserved. Wine's
 *     preloader asks for gigabytes of it before it does anything else.
 *   - mprotect(PROT_NONE) and back has to give the data back. An
 *     implementation that freed the frame passes any test that does not
 *     look, and made Wine read its own heap headers out of zeroes.
 *   - MAP_FIXED_NOREPLACE - "this address or nothing" - was ignored, so a
 *     loader that asked for a Windows image's base address got an
 *     arbitrary one and a confusing error.
 *
 * And one about files rather than memory: a program may open a file,
 * unlink it, and keep using the descriptor. It is how anonymous shared
 * memory is made on Unix, and wineserver does exactly that.
 */

static long sc1(long n, long a) {
    long r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "a"(n), "b"(a) : "memory");
    return r;
}
static long sc2(long n, long a, long b) {
    long r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "a"(n), "b"(a), "c"(b)
                         : "memory");
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
    __asm__ __volatile__("push %%ebp\n\t"
                         "mov %7, %%ebp\n\t"
                         "int $0x80\n\t"
                         "pop %%ebp"
                         : "=a"(r)
                         : "a"(n), "b"(a), "c"(b), "d"(c), "S"(d), "D"(e),
                           "rm"(f)
                         : "memory");
    return r;
}
static long sc0(long n) {
    long r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "a"(n) : "memory");
    return r;
}

#define SYS_exit       1
#define SYS_fork       2
#define SYS_read       3
#define SYS_write      4
#define SYS_open       5
#define SYS_close      6
#define SYS_waitpid    7
#define SYS_unlink    10
#define SYS_lseek     19
#define SYS_munmap    91
#define SYS_mprotect 125
#define SYS_ftruncate 93
#define SYS_mmap2    192

#define O_RDONLY 0
#define O_RDWR   2
#define O_CREAT  0x40
#define O_TRUNC  0x200

#define PROT_NONE  0
#define PROT_READ  1
#define PROT_WRITE 2

#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_FIXED     0x10
#define MAP_ANONYMOUS 0x20
#define MAP_FIXED_NOREPLACE 0x100000

#define EEXIST_ (-17)
#define PAGE 4096

static unsigned slen(const char* s) { unsigned n = 0; while (s[n]) n++; return n; }
static void out(const char* s) { sc3(SYS_write, 1, (long)s, slen(s)); }
static void check(const char* label, int ok) {
    out(ok ? "  [ok]   " : "  [FAIL] ");
    out(label);
    out("\n");
}
static int memeq(const char* a, const char* b, unsigned n) {
    for (unsigned i = 0; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}
static void fill(char* p, char c, unsigned n) {
    for (unsigned i = 0; i < n; i++) p[i] = c;
}

static long mmap2(long addr, long len, long prot, long flags, long fd,
                  long pgoff) {
    return sc6(SYS_mmap2, addr, len, prot, flags, fd, pgoff);
}

/* mmap returns an address, and on Linux that address routinely has the
 * top bit set - `> 0` is the wrong test and would call a perfectly good
 * mapping at 0xf7xxxxxx a failure. An error is a small negative value. */
static int mapped(long r) { return (unsigned long)r < 0xfffff000UL; }

#define FILEPATH "/tmp/novaris_mmaptest"

int main_(void) {
    out("mmap test - written for Linux, running unmodified\n\n");
    sc1(SYS_unlink, (long)FILEPATH);

    char buf[64];
    int status = 0;

    /* 1. a shared file mapping is the file ------------------------------- */
    out("1. MAP_SHARED\n");
    long fd = sc3(SYS_open, (long)FILEPATH, O_RDWR | O_CREAT | O_TRUNC, 0600);
    check("a file to map", fd >= 0);
    check("sized before mapping", sc2(SYS_ftruncate, fd, 2 * PAGE) == 0);

    long a = mmap2(0, 2 * PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    check("mapped it shared", mapped(a));
    char* pa = (char*)a;

    fill(pa, 'A', 8);
    /* The mapping is the file, so a plain read() sees what the store
     * through the pointer wrote. This is the whole difference between
     * MAP_SHARED and MAP_PRIVATE, and the reason it cannot be faked with
     * a copy. */
    sc3(SYS_lseek, fd, 0, 0);
    long n = sc3(SYS_read, fd, (long)buf, 8);
    check("a store through it is visible through read()",
          n == 8 && memeq(buf, "AAAAAAAA", 8));

    /* 2. two mappings of one file are one memory ------------------------- */
    out("\n2. two mappings, one file\n");
    long b = mmap2(0, 2 * PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    check("mapped the same file twice", mapped(b) && b != a);
    char* pb = (char*)b;
    fill(pa, 'B', 8);
    check("a store through one is visible through the other",
          memeq(pb, "BBBBBBBB", 8));

    /* 3. and across a fork ----------------------------------------------- */
    out("\n3. shared across fork\n");
    {
        long pid = sc0(SYS_fork);
        if (pid == 0) {
            fill(pa, 'C', 8);
            sc1(SYS_exit, 0);
        }
        sc3(SYS_waitpid, pid, (long)&status, 0);
        check("what the child wrote through it, the parent sees",
              memeq(pa, "CCCCCCCC", 8));
    }

    /* 3a. offsets, growth, and a second *process* ------------------------
     *
     * Milestone 32. This is the exact shape of wineserver's session
     * mapping, and it is here because Wine failed on it while every test
     * above passed. The server grows a file with ftruncate, maps only the
     * newly added tail at a non-zero offset, and keeps every earlier
     * block mapped where it was; clients then map their own views of the
     * same file, at offsets of their own, and must see what the server
     * wrote. Nothing above tests a non-zero offset, growth of a file that
     * is already mapped, or two mappings made by two different processes
     * rather than inherited across a fork. */
    out("\n3a. offsets, growth, and a second process\n");
    {
        check("grow the file to four pages",
              sc2(SYS_ftruncate, fd, 4 * PAGE) == 0);

        /* The earlier mappings must still be the file after it grew. */
        fill(pa, 'E', 8);
        check("a mapping made before the growth still works",
              memeq(pb, "EEEEEEEE", 8));

        /* A mapping at a non-zero offset is a different part of the file,
         * not another view of the start. */
        long t = mmap2(0, PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 2);
        check("mapped at a page offset", mapped(t));
        char* pt = (char*)t;
        fill(pt, 'F', 8);
        check("it is not an alias of offset zero", memeq(pa, "EEEEEEEE", 8));

        sc3(SYS_lseek, fd, 2 * PAGE, 0);
        n = sc3(SYS_read, fd, (long)buf, 8);
        check("and read() at that offset sees what it wrote",
              n == 8 && memeq(buf, "FFFFFFFF", 8));

        /* Grow again, with three mappings outstanding, and map the new
         * tail the way the server does. */
        check("grow it again while mapped",
              sc2(SYS_ftruncate, fd, 8 * PAGE) == 0);
        long u = mmap2(0, PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 6);
        check("mapped the new tail", mapped(u));
        fill((char*)u, 'G', 8);

        check("the mappings from before the second growth still hold",
              memeq(pa, "EEEEEEEE", 8) && memeq(pt, "FFFFFFFF", 8));

        /* And now the client half: a *separate* process that opens the
         * file itself and maps its own view, rather than inheriting one.
         * What it writes has to arrive in a mapping this process made
         * before that process existed. */
        long pid = sc0(SYS_fork);
        if (pid == 0) {
            long cfd = sc3(SYS_open, (long)FILEPATH, O_RDWR, 0);
            long cm = mmap2(0, PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, cfd, 2);
            if (!mapped(cm)) sc1(SYS_exit, 2);
            fill((char*)cm, 'H', 8);
            sc1(SYS_exit, memeq((char*)cm, "HHHHHHHH", 8) ? 0 : 3);
        }
        sc3(SYS_waitpid, pid, (long)&status, 0);
        check("the child mapped the same file by name", (status >> 8) == 0);
        check("and what it wrote is visible in a mapping made earlier",
              memeq(pt, "HHHHHHHH", 8));

        sc2(SYS_munmap, t, PAGE);
        sc2(SYS_munmap, u, PAGE);
        /* Back to what section 4 expects to find at offset 0. */
        fill(pa, 'C', 8);
    }

    /* 4. a private mapping is not shared ---------------------------------- */
    out("\n4. MAP_PRIVATE is private\n");
    {
        long c = mmap2(0, PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
        check("mapped the same file privately", mapped(c));
        char* pc = (char*)c;
        check("it starts as a copy of the file", memeq(pc, "CCCCCCCC", 8));
        fill(pc, 'D', 8);
        check("a store through it does not reach the file",
              memeq(pa, "CCCCCCCC", 8));
        sc2(SYS_munmap, c, PAGE);
    }

    sc2(SYS_munmap, a, 2 * PAGE);
    sc2(SYS_munmap, b, 2 * PAGE);

    /* 5. reservations ----------------------------------------------------- */
    out("\n5. PROT_NONE reserves\n");
    {
        /* A whole megabyte of address space and no memory. On a machine
         * with less than a megabyte free this still has to work, which is
         * the point - it is address space, not memory. */
        long r = mmap2(0, 256 * PAGE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS,
                       -1, 0);
        check("reserved a megabyte with PROT_NONE", mapped(r));

        /* "This address or nothing" must refuse an address that is
         * already spoken for, and a reservation counts. */
        long again = mmap2(r, PAGE, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
                           -1, 0);
        check("MAP_FIXED_NOREPLACE refuses a reserved address",
              again == EEXIST_);

        /* MAP_FIXED takes it anyway, which is the difference between the
         * two flags. */
        long got = mmap2(r, PAGE, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        check("MAP_FIXED takes it", got == r);
        char* pr = (char*)r;
        check("and the memory reads as zero", pr[0] == 0 && pr[PAGE - 1] == 0);

        /* The rest of the reservation becomes memory through mprotect,
         * which is how a program cashes in address space it kept. */
        check("mprotect turns the rest into memory",
              sc3(SYS_mprotect, r + PAGE, 8 * PAGE,
                  PROT_READ | PROT_WRITE) == 0);
        fill(pr + PAGE, 'E', 8);
        check("and it can be written and read back",
              memeq(pr + PAGE, "EEEEEEEE", 8));

        /* And back again, and back. The data must survive the round trip:
         * PROT_NONE takes away the *permission*, not the page. */
        check("mprotect back to PROT_NONE",
              sc3(SYS_mprotect, r + PAGE, 8 * PAGE, PROT_NONE) == 0);
        check("and back to readable",
              sc3(SYS_mprotect, r + PAGE, 8 * PAGE,
                  PROT_READ | PROT_WRITE) == 0);
        check("the bytes are still there",
              memeq(pr + PAGE, "EEEEEEEE", 8));

        sc2(SYS_munmap, r, 256 * PAGE);
    }

    /* 6. a file outlives its name ----------------------------------------- */
    out("\n6. unlink with the file still open\n");
    {
        long f = sc3(SYS_open, (long)FILEPATH, O_RDWR | O_TRUNC, 0600);
        check("opened it again", f >= 0);
        sc3(SYS_write, f, (long)"still here", 10);
        check("unlinked while open", sc1(SYS_unlink, (long)FILEPATH) == 0);
        check("the name is gone",
              sc3(SYS_open, (long)FILEPATH, O_RDONLY, 0) < 0);

        sc3(SYS_lseek, f, 0, 0);
        long got = sc3(SYS_read, f, (long)buf, 10);
        buf[10] = '\0';
        check("but the descriptor still reads it",
              got == 10 && memeq(buf, "still here", 10));
        sc1(SYS_close, f);
        check("and after the last close it is really gone",
              sc3(SYS_open, (long)FILEPATH, O_RDONLY, 0) < 0);
    }

    sc1(SYS_close, fd);
    sc1(SYS_unlink, (long)FILEPATH);

    out("\nmmap test done.\n");
    return 0;
}

void _start(void) {
    sc1(SYS_exit, main_());
    __builtin_unreachable();
}
