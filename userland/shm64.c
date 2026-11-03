/* shm64.c - a file with no name, shared between two processes.
 *
 * Milestone 78 owed this test and did not have it. The five fixes it
 * made were covered only by the existing suite continuing to pass and by
 * Wine getting further, which is evidence and not an assertion.
 *
 * The two mechanisms worth asserting are the two that were wrong:
 *
 *   - A file unlinked while open goes on existing until the last
 *     descriptor closes. ramfs64_unlink used to free it immediately and
 *     its own comment predicted the consequence.
 *
 *   - A MAP_SHARED mapping is shared. A private copy satisfies every
 *     read and write a single process can make, so the only thing that
 *     can tell the difference is a second process - which is why the
 *     interesting half of this program runs after a fork.
 *
 * Together they are one idiom rather than two features: create, size,
 * unlink, map is how a program gets anonymous shared memory out of a
 * filesystem, and it is what Wine does for every shared mapping it
 * makes. Neither half is any use without the other.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/uio.h>
#include <sys/syscall.h>
#include <signal.h>
#include <sched.h>

#define NAME "/tmp/shm64-file"
#define SPAN 4096

static int failures;

static void ok(const char *what, int cond)
{
    printf("%-4s %s\n", cond ? "ok" : "FAIL", what);
    if (!cond)
        failures++;
}

int main(void)
{
    struct stat st;
    char buf[64];
    int fd;

    setvbuf(stdout, NULL, _IONBF, 0);
    unlink(NAME);

    /* --- a file that outlives its name ------------------------------- */

    fd = open(NAME, O_RDWR | O_CREAT | O_EXCL, 0600);
    ok("a file to share", fd >= 0);
    ok("with something in it", write(fd, "before", 6) == 6);

    ok("unlink removes the name", unlink(NAME) == 0);
    ok("and the name is gone", stat(NAME, &st) == -1 && errno == ENOENT);

    /* The descriptor is now the only thing that refers to it. Everything
     * below would read freed memory on a filesystem that let the node go
     * when the name did. */
    ok("but the descriptor still finds it", fstat(fd, &st) == 0);
    ok("at the size it had", st.st_size == 6);

    memset(buf, 0, sizeof(buf));
    ok("and still reads what was written",
       pread(fd, buf, sizeof(buf), 0) == 6 && memcmp(buf, "before", 6) == 0);

    ok("it can still be written", pwrite(fd, "after ", 6, 0) == 6);
    memset(buf, 0, sizeof(buf));
    ok("and read back changed",
       pread(fd, buf, sizeof(buf), 0) == 6 && memcmp(buf, "after ", 6) == 0);

    /* Sized after being unlinked, which is the order the idiom uses. */
    ok("and resized", ftruncate(fd, SPAN) == 0);
    ok("to the size asked for", fstat(fd, &st) == 0 && st.st_size == SPAN);

    /* --- and is shared, which only a second process can show --------- */
    {
        volatile unsigned char *p;
        int status = 0;
        pid_t kid;

        p = mmap(NULL, SPAN, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        ok("it maps shared", p != MAP_FAILED);

        /* What write(2) put there is what the mapping shows: one file,
         * one copy of its bytes, whichever way it is reached. */
        ok("the mapping shows what write(2) wrote",
           memcmp((void *)p, "after ", 6) == 0);

        /* And the other way round. */
        memcpy((void *)p + 16, "through-the-map", 15);
        memset(buf, 0, sizeof(buf));
        ok("and read(2) shows what the mapping wrote",
           pread(fd, buf, 15, 16) == 15 &&
           memcmp(buf, "through-the-map", 15) == 0);

        p[100] = 0xA5;

        kid = fork();
        if (kid == 0) {
            /* The child's own mapping of the same descriptor. A private
             * copy would give it the parent's bytes at this point and
             * diverge from here on - which is exactly what makes the
             * assertions below the ones that matter. */
            volatile unsigned char *q =
                mmap(NULL, SPAN, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            if (q == MAP_FAILED) _exit(61);

            if (q[100] != 0xA5) _exit(62);      /* the parent's store */
            q[200] = 0x5A;                      /* one of its own     */

            /* Through its inherited descriptor rather than its mapping,
             * so the child proves both routes reach the same bytes. */
            if (pwrite(fd, "child", 5, 300) != 5) _exit(63);
            _exit(23);
        }
        ok("fork returned", kid > 0);
        waitpid(kid, &status, 0);
        ok("the child saw the parent's store through its own mapping",
           WIFEXITED(status) && WEXITSTATUS(status) == 23);
        if (WIFEXITED(status) && WEXITSTATUS(status) != 23)
            printf("     (child exited %d)\n", WEXITSTATUS(status));

        /* The assertion a private copy fails. The child is gone; what it
         * wrote has to still be here, in this process's mapping. */
        ok("and the parent sees what the child wrote", p[200] == 0x5A);
        ok("including what it wrote through the descriptor",
           memcmp((void *)p + 300, "child", 5) == 0);

        ok("it unmaps", munmap((void *)p, SPAN) == 0);
    }

    /* --- a fixed shared mapping goes exactly where it was asked ------ */
    /* Wine maps its shared user data at an address compiled into every
     * Windows program, inside a range it reserved earlier. A mapping
     * that succeeded somewhere else would be worse than one that failed:
     * the call reports an address nobody is going to look at. */
    {
        void *reserved, *fixed;

        reserved = mmap(NULL, SPAN * 4, PROT_NONE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        ok("a range to reserve", reserved != MAP_FAILED);

        /* Over the middle of it, the way Wine does. */
        fixed = mmap((char *)reserved + SPAN, SPAN, PROT_READ | PROT_WRITE,
                     MAP_SHARED | MAP_FIXED, fd, 0);
        ok("a fixed shared mapping succeeds", fixed != MAP_FAILED);
        ok("at exactly the address asked for",
           fixed == (void *)((char *)reserved + SPAN));
        ok("and it is the same file",
           fixed != MAP_FAILED && ((unsigned char *)fixed)[200] == 0x5A);

        munmap(reserved, SPAN * 4);
    }

    close(fd);

    /* The name never came back. */
    ok("the name is still gone", stat(NAME, &st) == -1);

    /* --- process_vm_readv and process_vm_writev --------------------- *
     *
     * One process reading and writing another's memory, which is how
     * the wineserver does ReadProcessMemory and WriteProcessMemory. The
     * child's buffer is shared copy-on-write with the parent's after the
     * fork, so a write into it must land in the child alone; and a page
     * the child mapped and never touched must read as zeros and take a
     * write, which on a kernel that backs memory on first touch means
     * backing it in the child from outside. */
    {
        static char buf[8192];
        int go[2], back[2];
        pid_t kid;
        char *fresh, *ro;
        char got[16];
        struct iovec l, r;
        ssize_t n;
        int status;

        strcpy(buf, "parent's");
        ok("pipes for the remote-memory test", pipe(go) == 0 && pipe(back) == 0);
        fresh = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        ro = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        kid = fork();
        if (kid == 0) {
            char c;
            strcpy(buf, "child's!");
            if (write(back[1], "r", 1) != 1) _exit(10);
            if (read(go[0], &c, 1) != 1) _exit(11);
            /* What the parent wrote, here and only here. */
            if (strcmp(buf, "written!") != 0) _exit(1);
            if (strcmp(fresh, "into new") != 0) _exit(2);
            _exit(0);
        }
        {
            char c;
            ok("the child is ready", read(back[0], &c, 1) == 1);
        }

        l.iov_base = got; l.iov_len = 9;
        r.iov_base = buf; r.iov_len = 9;
        n = process_vm_readv(kid, &l, 1, &r, 1, 0);
        ok("process_vm_readv reads the child's memory", n == 9);
        ok("and gets the child's bytes, not the parent's",
           strcmp(got, "child's!") == 0);

        l.iov_base = "written!"; l.iov_len = 9;
        n = process_vm_writev(kid, &l, 1, &r, 1, 0);
        ok("process_vm_writev writes into it", n == 9);
        ok("and the parent's own copy is untouched",
           strcmp(buf, "parent's") == 0);

        memset(got, 'x', sizeof(got));
        l.iov_base = got; l.iov_len = 8;
        r.iov_base = fresh; r.iov_len = 8;
        n = process_vm_readv(kid, &l, 1, &r, 1, 0);
        ok("a page the child never touched reads", n == 8);
        ok("as zeros", got[0] == 0 && got[7] == 0);
        l.iov_base = "into new"; l.iov_len = 9;
        r.iov_base = fresh; r.iov_len = 9;
        ok("and takes a write", process_vm_writev(kid, &l, 1, &r, 1, 0) == 9);

        l.iov_base = "nope"; l.iov_len = 4;
        r.iov_base = ro; r.iov_len = 4;
        errno = 0;
        n = process_vm_writev(kid, &l, 1, &r, 1, 0);
        ok("a read-only page in the child is not written",
           n == -1 && errno == EFAULT);

        ok("the child sees what was written, and only that",
           write(go[1], "g", 1) == 1 &&
           waitpid(kid, &status, 0) == kid &&
           WIFEXITED(status) && WEXITSTATUS(status) == 0);

        errno = 0;
        l.iov_base = got; l.iov_len = 1;
        r.iov_base = buf; r.iov_len = 1;
        n = process_vm_readv(kid, &l, 1, &r, 1, 0);
        ok("and once it is gone, ESRCH", n == -1 && errno == ESRCH);
        munmap(fresh, 4096);
        munmap(ro, 4096);
    }

    /* --- fsync ------------------------------------------------------ *
     *
     * SQLite calls it at every commit and treats a failure as an I/O
     * error, so a kernel without it has no working database. */
    {
        int fd = open("/tmp/shm64-sync", O_CREAT | O_RDWR | O_TRUNC, 0644);
        ok("a file to sync", fd >= 0 && write(fd, "x", 1) == 1);
        ok("fsync succeeds", fsync(fd) == 0);
        ok("and fdatasync", fdatasync(fd) == 0);
        close(fd);
        errno = 0;
        ok("on a closed descriptor it is EBADF",
           fsync(fd) == -1 && errno == EBADF);
        unlink("/tmp/shm64-sync");
    }

    /* --- a shared view from the middle of a file ---------------------- *
     *
     * Wine maps a view of a section at an offset with MAP_SHARED, and
     * Chromium's IPC is views of shared sections. This kernel refused any
     * offset, so Wine fell back to reading the bytes into private memory -
     * a copy that never saw the other side's writes - and a headless
     * Chromium's children rejected their messages and quit. */
    {
        int fd = open("/tmp/shm64-view", O_CREAT | O_RDWR | O_TRUNC, 0600);
        char *whole, *view;
        int status = 0;
        pid_t kid;

        ok("a three-page file", fd >= 0 && ftruncate(fd, 3 * 4096) == 0);
        whole = mmap(NULL, 3 * 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        view  = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 4096);
        ok("maps whole and from its second page",
           whole != MAP_FAILED && view != MAP_FAILED);
        if (whole != MAP_FAILED && view != MAP_FAILED) {
            strcpy(view, "through the view");
            ok("a write through the view lands at the offset",
               !strcmp(whole + 4096, "through the view"));
            kid = fork();
            if (kid == 0) {
                strcpy(whole + 4096 + 100, "from the child");
                _exit(0);
            }
            ok("and the child's write shows through the parent's view",
               waitpid(kid, &status, 0) == kid &&
               !strcmp(view + 100, "from the child"));
            munmap(whole, 3 * 4096);
            munmap(view, 4096);
        }
        close(fd);
        unlink("/tmp/shm64-view");
    }

    /* --- private futexes stay in their process ---------------------- *
     *
     * After a fork both processes have a word at the same address, and
     * as private futexes they are two different words. The kernel used
     * to match a wake on the address alone, so a FUTEX_WAKE(addr, 1) in
     * one process could be spent on a thread of another sleeping at the
     * same address. Wine puts ntdll at one address in every process, and
     * a headless Chromium's browser thread slept forever on a wake that
     * had gone to a renderer. */
    {
        static volatile int word;
        int p[2], status = 0;
        char c;
        long n;
        pid_t kid;

        ok("a pipe to hear the child start", pipe(p) == 0);
        kid = fork();
        if (kid == 0) {
            close(p[0]);
            word = 0;
            if (write(p[1], "w", 1) != 1) _exit(4);
            /* FUTEX_WAIT | FUTEX_PRIVATE_FLAG, forever. */
            syscall(SYS_futex, &word, 0 | 128, 0, NULL, NULL, 0);
            _exit(3);   /* only a wake meant for someone else gets here */
        }
        close(p[1]);
        ok("the child is about to wait", read(p[0], &c, 1) == 1);
        for (int i = 0; i < 200; i++) sched_yield();

        /* FUTEX_WAKE | FUTEX_PRIVATE_FLAG on this process's own word. */
        n = syscall(SYS_futex, &word, 1 | 128, 1, NULL, NULL, 0);
        ok("a private wake finds no waiter in another process", n == 0);
        for (int i = 0; i < 200; i++) sched_yield();
        ok("and the child is still asleep",
           waitpid(kid, &status, WNOHANG) == 0);
        kill(kid, SIGKILL);
        ok("until it is killed",
           waitpid(kid, &status, 0) == kid && WIFSIGNALED(status));
        close(p[0]);
    }

    printf("shm64: %d failures\n", failures);
    return failures ? 1 : 119;
}
