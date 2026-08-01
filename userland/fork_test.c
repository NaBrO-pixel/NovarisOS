/* fork_test.c - a Linux program that forks, execs, pipes and polls, run
 * unmodified on Novaris.
 *
 * Same rules as the rest of the comparison binaries: raw `int $0x80`,
 * Linux syscall numbers, `gcc -m32 -static -nostdlib -ffreestanding`,
 * linked against nothing. The same bytes run on the Linux build host and
 * on Novaris and the two transcripts are compared, so every line below is
 * a claim that Novaris behaves the way Linux does - not merely that it
 * does something.
 *
 * What it is for. ROADMAP.md Milestone 28 ended by naming the four things
 * that had to exist before Wine could start a wineserver: per-process
 * POSIX state, fork, execve and waitpid. Wine's start_server() is
 * literally those last three in a row, and its server loop multiplexes
 * descriptors rather than blocking on one, which is poll. So this file is
 * the checklist.
 *
 * Two rules keep the transcript deterministic on a preemptive kernel:
 *
 *   - only the parent prints. A child reports what it saw by writing into
 *     a pipe, and the parent reads it and prints the verdict, so nothing
 *     depends on which of the two the scheduler runs first.
 *   - the one exception is the exec'd image, which prints a single line
 *     while the parent is blocked in waitpid and therefore cannot
 *     interleave with it.
 *
 * The exec'd image is *this same binary*, re-entered through
 * /proc/self/exe with a marker argument - which is how a program that
 * cannot know its own path re-executes itself, and works identically on
 * both systems.
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
/* wait4 takes four arguments, and the fourth is a pointer the kernel
 * writes through when it is not null. Passing three and leaving esi to
 * whatever happened to be in it is a real bug and not a theoretical one:
 * Linux takes the garbage as a struct rusage* and answers -EFAULT. */
static long sc4(long n, long a, long b, long c, long d) {
    long r;
    __asm__ __volatile__("int $0x80"
                         : "=a"(r)
                         : "a"(n), "b"(a), "c"(b), "d"(c), "S"(d)
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
#define SYS_execve    11
#define SYS_chdir     12
#define SYS_mkdir     39
#define SYS_rmdir     40
#define SYS_pipe      42
#define SYS_dup2      63
#define SYS_getpid    20
#define SYS_getppid   64
#define SYS_wait4    114
#define SYS_poll     168
#define SYS_getcwd   183

#define O_RDONLY 0
#define O_WRONLY 1
#define O_CREAT  0x40
#define O_TRUNC  0x200

#define POLLIN   0x001
#define POLLOUT  0x004
#define POLLHUP  0x010
#define WNOHANG  1
#define ECHILD_  (-10)

struct pollfd { int fd; short events; short revents; };

/* --- a few bytes of libc ------------------------------------------------- */

static unsigned slen(const char* s) { unsigned n = 0; while (s[n]) n++; return n; }
static void out(const char* s) { sc3(SYS_write, 1, (long)s, slen(s)); }
static void check(const char* label, int ok) {
    out(ok ? "  [ok]   " : "  [FAIL] ");
    out(label);
    out("\n");
}
static int streq(const char* a, const char* b) {
    while (*a && *b) { if (*a++ != *b++) return 0; }
    return *a == *b;
}

/* Reads exactly `n` bytes, or as many as arrive before end of stream. A
 * pipe is a stream: one write is not one read, and a test that assumed it
 * was would be testing the buffer size rather than the pipe. */
static int read_all(int fd, char* buf, int n) {
    int got = 0;
    while (got < n) {
        long r = sc3(SYS_read, fd, (long)(buf + got), n - got);
        if (r <= 0) break;
        got += (int)r;
    }
    return got;
}

/* The exit status a parent reads back is not the code the child passed:
 * it is that code in the high byte of a word whose low seven bits are
 * reserved for a signal number. Both systems encode it the same way and
 * that is part of what is being checked. */
#define WIFEXITED(s)   (((s) & 0x7f) == 0)
#define WEXITSTATUS(s) (((s) >> 8) & 0xff)

#define DIRPATH  "/tmp/novaris_forktest"
#define FILENAME "marker.txt"

/* Written before the fork and modified by the child. A fork gives the
 * child a *copy* of its parent's memory, so the parent must not see the
 * change - the single most important thing about fork, and the one a
 * shared-address-space implementation would get wrong while passing every
 * other check in this file. */
static char shared[32] = "parent";

static int child_of_exec(void) {
    out("  [ok]   the exec'd image runs, and got its argument\n");
    /* Descriptor 9 was wired to a pipe by the process that exec'd this
     * one. Descriptors survive execve - that is what makes redirection
     * possible - so writing to it here is read by the parent below. */
    sc3(SYS_write, 9, (long)"execed", 6);
    return 9;
}

int main_(int argc, char** argv) {
    if (argc > 1 && streq(argv[1], "--child")) return child_of_exec();

    out("fork test - written for Linux, running unmodified\n\n");

    int my_pid = (int)sc0(SYS_getpid);
    int fd[2];
    char buf[64];
    int status = 0;

    /* 1. fork, and what the two sides see -------------------------------- */
    out("1. fork\n");
    {
        if (sc1(SYS_pipe, (long)fd) != 0) { out("  pipe failed\n"); return 1; }

        long pid = sc0(SYS_fork);
        if (pid == 0) {
            /* The child. It writes what it can see and nothing else -
             * printing here would race with the parent. */
            char msg[24];
            int me = (int)sc0(SYS_getpid);
            int dad = (int)sc0(SYS_getppid);
            msg[0] = (me != my_pid) ? 'y' : 'n';        /* a pid of my own */
            msg[1] = (dad == my_pid) ? 'y' : 'n';       /* and my parent's */
            msg[2] = streq(shared, "parent") ? 'y' : 'n'; /* memory inherited */
            shared[0] = 'C';                             /* now change it */
            msg[3] = (shared[0] == 'C') ? 'y' : 'n';
            sc3(SYS_write, fd[1], (long)msg, 4);
            sc1(SYS_close, fd[1]);
            sc1(SYS_exit, 7);
        }

        check("fork returns a positive pid to the parent", pid > 0);
        sc1(SYS_close, fd[1]);
        int n = read_all(fd[0], buf, 4);
        check("the child reported back through a pipe", n == 4);
        check("the child has a pid of its own", buf[0] == 'y');
        check("the child's parent is this process", buf[1] == 'y');
        check("the child inherited this process's memory", buf[2] == 'y');
        check("and could write to its own copy of it", buf[3] == 'y');
        check("the parent's copy is untouched", streq(shared, "parent"));

        long got = sc3(SYS_waitpid, pid, (long)&status, 0);
        check("waitpid returns the pid that exited", got == pid);
        check("the child exited rather than being signalled", WIFEXITED(status));
        check("with the status it passed to exit", WEXITSTATUS(status) == 7);
        sc1(SYS_close, fd[0]);
    }

    /* 2. end of stream, which is what makes a pipe a protocol ------------- */
    out("\n2. pipes\n");
    {
        sc1(SYS_pipe, (long)fd);
        sc3(SYS_write, fd[1], (long)"hello pipe", 10);
        int n = read_all(fd[0], buf, 10);
        buf[10] = '\0';
        check("a pipe carries bytes", n == 10 && streq(buf, "hello pipe"));

        /* The writer closing is not an error, it is the end of the data -
         * the distinction every protocol loop is built on. */
        sc1(SYS_close, fd[1]);
        long r = sc3(SYS_read, fd[0], (long)buf, sizeof(buf));
        check("closing the write end is end of stream, not an error", r == 0);
        sc1(SYS_close, fd[0]);
    }

    /* 3. dup2 ------------------------------------------------------------- */
    out("\n3. dup2\n");
    {
        sc1(SYS_pipe, (long)fd);
        long r = sc2(SYS_dup2, fd[1], 9);
        check("dup2 returns the descriptor it was asked for", r == 9);
        sc3(SYS_write, 9, (long)"through 9", 9);
        int n = read_all(fd[0], buf, 9);
        buf[9] = '\0';
        check("writing to the duplicate reaches the same pipe",
              n == 9 && streq(buf, "through 9"));

        /* Two descriptors, one object: closing one must leave the other
         * working, and end of stream must wait for the *last* of them. */
        sc1(SYS_close, 9);
        sc3(SYS_write, fd[1], (long)"still here", 10);
        n = read_all(fd[0], buf, 10);
        buf[10] = '\0';
        check("closing the duplicate leaves the original open",
              n == 10 && streq(buf, "still here"));
        sc1(SYS_close, fd[1]);
        sc1(SYS_close, fd[0]);
    }

    /* 4. poll ------------------------------------------------------------- */
    out("\n4. poll\n");
    {
        sc1(SYS_pipe, (long)fd);
        struct pollfd p;
        p.fd = fd[0];
        p.events = POLLIN;
        p.revents = 0;

        long r = sc3(SYS_poll, (long)&p, 1, 0);
        check("an empty pipe is not ready, and poll says so at once", r == 0);

        sc3(SYS_write, fd[1], (long)"x", 1);
        p.revents = 0;
        r = sc3(SYS_poll, (long)&p, 1, 0);
        check("a pipe with a byte in it is readable",
              r == 1 && (p.revents & POLLIN));
        read_all(fd[0], buf, 1);

        /* The blocking path: nothing to read until a *different process*
         * writes, so poll has to actually wait rather than spin. */
        long pid = sc0(SYS_fork);
        if (pid == 0) {
            sc3(SYS_write, fd[1], (long)"woken", 5);
            sc1(SYS_exit, 0);
        }
        p.revents = 0;
        r = sc3(SYS_poll, (long)&p, 1, 5000);
        check("poll waits, and a write from another process wakes it",
              r == 1 && (p.revents & POLLIN));
        int n = read_all(fd[0], buf, 5);
        buf[5] = '\0';
        check("and the bytes that woke it are there",
              n == 5 && streq(buf, "woken"));
        sc3(SYS_waitpid, pid, (long)&status, 0);

        /* Every writer gone is POLLHUP, which is how a server loop finds
         * out a client left without having subscribed to the possibility. */
        sc1(SYS_close, fd[1]);
        p.events = POLLIN;
        p.revents = 0;
        r = sc3(SYS_poll, (long)&p, 1, 0);
        check("a pipe with no writer left reports hangup",
              r == 1 && (p.revents & (POLLIN | POLLHUP)));
        sc1(SYS_close, fd[0]);
    }

    /* 5. execve ----------------------------------------------------------- */
    out("\n5. execve\n");
    {
        sc1(SYS_pipe, (long)fd);
        long pid = sc0(SYS_fork);
        if (pid == 0) {
            /* Wire the pipe onto a descriptor the exec'd image knows
             * about, then become a different program. Descriptors survive
             * execve; everything else about this process does not. */
            sc2(SYS_dup2, fd[1], 9);
            sc1(SYS_close, fd[0]);
            sc1(SYS_close, fd[1]);
            static char* const av[] = { "forktest", "--child", 0 };
            static char* const ev[] = { 0 };
            sc3(SYS_execve, (long)"/proc/self/exe", (long)av, (long)ev);
            sc1(SYS_exit, 111);   /* only reached if execve failed */
        }
        sc1(SYS_close, fd[1]);
        int n = read_all(fd[0], buf, 6);
        buf[6] = '\0';
        long got = sc3(SYS_waitpid, pid, (long)&status, 0);
        check("the exec'd image kept the descriptor it was given",
              n == 6 && streq(buf, "execed"));
        check("and exited with its own status",
              got == pid && WIFEXITED(status) && WEXITSTATUS(status) == 9);
        sc1(SYS_close, fd[0]);
    }

    /* 6. waiting, and not waiting ----------------------------------------- */
    out("\n6. wait4 and WNOHANG\n");
    {
        long pid = sc0(SYS_fork);
        if (pid == 0) sc1(SYS_exit, 3);

        /* WNOHANG means "tell me now"; the answer is 0 until the child
         * has actually finished, and the pid once it has. Which of those
         * the first call gets is a scheduling detail, so the loop is what
         * is portable - and it must terminate. */
        long got = 0;
        for (int i = 0; i < 100000 && got == 0; i++) {
            got = sc4(SYS_wait4, -1, (long)&status, WNOHANG, 0);
        }
        check("wait4 with WNOHANG eventually reports the child", got == pid);
        check("with its status", WIFEXITED(status) && WEXITSTATUS(status) == 3);

        got = sc4(SYS_wait4, -1, (long)&status, 0, 0);
        check("waiting again with no children left is ECHILD",
              got == ECHILD_);
    }

    /* 7. a working directory, per process ---------------------------------- */
    out("\n7. the working directory\n");
    {
        sc1(SYS_rmdir, (long)DIRPATH);
        long r = sc2(SYS_mkdir, (long)DIRPATH, 0755);
        check("a directory to work in", r == 0);
        check("chdir into it", sc1(SYS_chdir, (long)DIRPATH) == 0);

        r = sc2(SYS_getcwd, (long)buf, sizeof(buf));
        check("getcwd reports where we went",
              r == (long)slen(DIRPATH) + 1 && streq(buf, DIRPATH));

        /* And a relative path now resolves against it, which is the whole
         * point of having one. */
        long f = sc3(SYS_open, (long)FILENAME, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        check("a relative path resolves against it", f >= 0);
        sc3(SYS_write, f, (long)"here", 4);
        sc1(SYS_close, f);

        f = sc3(SYS_open, (long)(DIRPATH "/" FILENAME), O_RDONLY, 0);
        int n = f >= 0 ? read_all((int)f, buf, 4) : 0;
        buf[4] = '\0';
        check("and names the same file the absolute path does",
              n == 4 && streq(buf, "here"));
        sc1(SYS_close, f);

        /* The child inherits it, and moving it back does not move the
         * parent's - a working directory belongs to a process. */
        sc1(SYS_pipe, (long)fd);
        long pid = sc0(SYS_fork);
        if (pid == 0) {
            char cwd[64];
            sc2(SYS_getcwd, (long)cwd, sizeof(cwd));
            char msg[2];
            msg[0] = streq(cwd, DIRPATH) ? 'y' : 'n';
            sc1(SYS_chdir, (long)"/");
            sc2(SYS_getcwd, (long)cwd, sizeof(cwd));
            msg[1] = streq(cwd, "/") ? 'y' : 'n';
            sc3(SYS_write, fd[1], (long)msg, 2);
            sc1(SYS_exit, 0);
        }
        sc1(SYS_close, fd[1]);
        read_all(fd[0], buf, 2);
        sc3(SYS_waitpid, pid, (long)&status, 0);
        check("the child inherited the working directory", buf[0] == 'y');
        check("and could move its own", buf[1] == 'y');
        sc2(SYS_getcwd, (long)buf, sizeof(buf));
        check("without moving the parent's", streq(buf, DIRPATH));
        sc1(SYS_close, fd[0]);

        sc1(SYS_chdir, (long)"/");
        sc1(SYS_unlink, (long)(DIRPATH "/" FILENAME));
        sc1(SYS_rmdir, (long)DIRPATH);
    }

    out("\nfork test done.\n");
    return 0;
}

/* argc and argv live on the stack the kernel built, and a freestanding
 * program has to go and get them: esp points at argc on entry, with the
 * argv pointers immediately above it. */
__asm__(
    ".globl _start\n"
    "_start:\n"
    "    mov %esp, %eax\n"
    "    and $-16, %esp\n"
    "    push %eax\n"
    "    call start_c\n"
);

void start_c(long* sp) {
    int argc = (int)sp[0];
    char** argv = (char**)(sp + 1);
    sc1(SYS_exit, main_(argc, argv));
    __builtin_unreachable();
}
