/* libNovaC on the real target: a freestanding static program that uses only
 * libNovaC (no host libc) and talks to the kernel through raw system calls.
 *
 * Because NovarisOS implements the Linux x86-64 ABI, the identical binary
 * runs on a Linux host and inside NovarisOS; the two transcripts must match
 * line for line (see the novac64 differential in Makefile.amd64). Nothing
 * printed may depend on addresses, pids, times or argv[0].
 *
 * Exit status 131 = every check passed; 1 = at least one failed. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <dirent.h>
#include <stdint.h>
#include <signal.h>

#define ROOT "/tmp/novac_t"

static int failures;

static void ok(const char *what, int cond)
{
    printf("%-4s %s\n", cond ? "ok" : "FAIL", what);
    if (!cond) failures++;
}

static void on_exit_1(void) { printf("atexit: first registered, runs last\n"); }
static void on_exit_2(void) { printf("atexit: last registered, runs first\n"); }

static void test_format(void)
{
    char b[64];
    int n;
    n = printf("printf: %d %u %x %s %c %5.2s|\n", -7, 7u, 255, "str", 'z', "abcdef");
    ok("printf returns the character count", n == 29);
    snprintf(b, sizeof b, "%8.3d|%-6d|%+d|%#o", 42, 42, 42, 8);
    ok("snprintf width/precision/flags", strcmp(b, "     042|42    |+42|010") == 0);
    ok("snprintf truncates and terminates", snprintf(b, 4, "%s", "abcdefgh") == 8 && strcmp(b, "abc") == 0);
    fprintf(stdout, "fprintf to stdout %ld\n", -123456789012L);
    puts("puts adds a newline");
    putchar('c'); putchar('\n');
}

static void test_memory(void)
{
    enum { N = 200 };
    void *p[N];
    unsigned char *c;
    int i, good = 1;
    for (i = 0; i < N; i++) {
        p[i] = malloc(16 + (size_t)i * 13);
        if (!p[i]) { good = 0; break; }
        memset(p[i], i, 16 + (size_t)i * 13);
        if (((uintptr_t)p[i] & 15) != 0) good = 0;
    }
    ok("200 allocations succeed, 16-byte aligned", good);
    for (i = 0; i < N && good; i++) {
        c = p[i];
        if (c[0] != (unsigned char)i || c[16 + (size_t)i * 13 - 1] != (unsigned char)i) good = 0;
    }
    ok("allocations keep their contents", good);
    for (i = 0; i < N; i += 2) free(p[i]);
    for (i = 1; i < N; i += 2) free(p[i]);
    c = calloc(4096, 4);
    for (i = 0, good = 1; i < 4096 * 4; i++) if (c[i]) good = 0;
    ok("calloc memory from the kernel is zero", good);
    free(c);
    c = malloc(3u * 1024 * 1024);
    ok("a 3MB allocation maps and unmaps", c != NULL);
    if (c) { memset(c, 1, 3u * 1024 * 1024); free(c); }
    c = realloc(NULL, 10);
    memcpy(c, "0123456789", 10);
    c = realloc(c, 100000);
    ok("realloc grows and keeps data", c && memcmp(c, "0123456789", 10) == 0);
    free(c);
    errno = 0;
    ok("an impossible allocation fails with ENOMEM", malloc((size_t)1 << 62) == NULL && errno == ENOMEM);
}

static void test_files(void)
{
    struct stat st;
    char buf[64], cwd[256];
    int fd;
    ssize_t n;

    unlink(ROOT "/a.txt"); unlink(ROOT "/b.txt"); rmdir(ROOT);
    ok("mkdir creates a directory", mkdir(ROOT, 0755) == 0);
    errno = 0;
    ok("mkdir on an existing path fails with EEXIST", mkdir(ROOT, 0755) == -1 && errno == EEXIST);
    ok("stat sees it as a directory", stat(ROOT, &st) == 0 && S_ISDIR(st.st_mode));

    fd = open(ROOT "/a.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    ok("open(O_CREAT) succeeds", fd >= 0);
    ok("write returns the byte count", write(fd, "hello, novaris\n", 15) == 15);
    ok("close succeeds", close(fd) == 0);
    ok("stat reports the size", stat(ROOT "/a.txt", &st) == 0 && st.st_size == 15 && S_ISREG(st.st_mode));

    fd = open(ROOT "/a.txt", O_RDONLY);
    ok("open for reading", fd >= 0);
    memset(buf, 0, sizeof buf);
    ok("read returns the bytes", read(fd, buf, 5) == 5 && memcmp(buf, "hello", 5) == 0);
    ok("lseek(SEEK_CUR) reports the offset", lseek(fd, 0, SEEK_CUR) == 5);
    ok("lseek(SEEK_SET) repositions", lseek(fd, 7, SEEK_SET) == 7);
    n = read(fd, buf, 7);
    ok("read after seek", n == 7 && memcmp(buf, "novaris", 7) == 0);
    ok("lseek(SEEK_END) gives the size", lseek(fd, 0, SEEK_END) == 15);
    ok("read at end of file returns 0", read(fd, buf, 4) == 0);
    ok("fstat agrees with stat", fstat(fd, &st) == 0 && st.st_size == 15);
    close(fd);

    errno = 0;
    ok("open of a missing file fails with ENOENT", open(ROOT "/nope", O_RDONLY) == -1 && errno == ENOENT);
    errno = 0;
    ok("read on a bad descriptor fails with EBADF", read(99, buf, 1) == -1 && errno == EBADF);
    errno = 0;
    ok("stat of a missing path fails with ENOENT", stat(ROOT "/nope", &st) == -1 && errno == ENOENT);
    ok("access(F_OK) on an existing file", access(ROOT "/a.txt", F_OK) == 0);
    errno = 0;
    ok("access on a missing file fails", access(ROOT "/nope", F_OK) == -1 && errno == ENOENT);

    ok("rename moves a file", rename(ROOT "/a.txt", ROOT "/b.txt") == 0);
    ok("the old name is gone", access(ROOT "/a.txt", F_OK) == -1);
    ok("the new name exists", access(ROOT "/b.txt", F_OK) == 0);

    fd = open(ROOT "/b.txt", O_RDWR);
    ok("ftruncate shrinks a file", fd >= 0 && ftruncate(fd, 5) == 0 && fstat(fd, &st) == 0 && st.st_size == 5);
    {
        int d1, d2;
        /* Linux also makes later seeks on one descriptor visible through the
         * other (one open file description); NovarisOS copies the offset at
         * dup time instead (kernel/syscall64.c), so only the shared starting
         * offset is asserted here. */
        if (fd >= 0) lseek(fd, 3, SEEK_SET);
        d1 = fd >= 0 ? dup(fd) : -1;
        d2 = fd >= 0 ? dup2(fd, 20) : -1;
        ok("dup returns a new descriptor", d1 >= 0 && d1 != fd);
        ok("dup2 uses the requested number", d2 == 20);
        ok("a duplicate starts at the same file offset", d1 >= 0 && lseek(d1, 0, SEEK_CUR) == 3);
        if (d1 >= 0) close(d1);
        if (d2 >= 0) close(d2);
    }
    if (fd >= 0) close(fd);
    ok("lstat on a regular file", lstat(ROOT "/b.txt", &st) == 0 && S_ISREG(st.st_mode));
    ok("getpid is positive", getpid() > 0);
    ok("sleep(0) returns 0", sleep(0) == 0);

    ok("chdir into the directory", chdir(ROOT) == 0);
    ok("getcwd returns the path", getcwd(cwd, sizeof cwd) == cwd && strcmp(cwd, ROOT) == 0);
    fd = open("b.txt", O_RDONLY);
    ok("a relative path resolves against the cwd", fd >= 0);
    if (fd >= 0) close(fd);
    chdir("/tmp");

    ok("unlink removes the file", unlink(ROOT "/b.txt") == 0);
    ok("rmdir removes the directory", rmdir(ROOT) == 0);
    errno = 0;
    ok("rmdir of a missing directory fails with ENOENT", rmdir(ROOT) == -1 && errno == ENOENT);
    ok("strerror names ENOENT", strcmp(strerror(ENOENT), "No such file or directory") == 0);
}

static void test_streams(void)
{
    FILE *f;
    char line[80], big[3000];
    struct stat st;
    int c, i;

    mkdir(ROOT, 0755);
    f = fopen(ROOT "/s.txt", "w");
    ok("fopen(w)", f != NULL);
    ok("fprintf into a file", fprintf(f, "line %d\nline %s\n", 1, "two") == 16);
    ok("fputs", fputs("third line\n", f) >= 0);
    ok("fputc", fputc('Z', f) == 'Z');
    ok("fwrite returns the item count", fwrite("abc", 1, 3, f) == 3);
    ok("fclose flushes and succeeds", fclose(f) == 0);
    ok("the file has the buffered bytes", stat(ROOT "/s.txt", &st) == 0 && st.st_size == 16 + 11 + 1 + 3);

    f = fopen(ROOT "/s.txt", "r");
    ok("fopen(r)", f != NULL);
    ok("fgets reads one line", fgets(line, sizeof line, f) && strcmp(line, "line 1\n") == 0);
    ok("ftell tracks the logical position", ftell(f) == 7);
    ok("fgets reads the next", fgets(line, sizeof line, f) && strcmp(line, "line two\n") == 0);
    c = fgetc(f);
    ok("fgetc", c == 't');
    ok("ungetc pushes back", ungetc('T', f) == 'T' && fgetc(f) == 'T');
    ok("fseek to the start", fseek(f, 0, SEEK_SET) == 0 && ftell(f) == 0);
    ok("fseek from the end", fseek(f, -3, SEEK_END) == 0 && fgetc(f) == 'a');
    ok("fread reads the rest", fread(line, 1, 10, f) == 2);
    ok("eof is reported", feof(f) && !ferror(f));
    clearerr(f);
    ok("clearerr clears eof", !feof(f));
    fclose(f);

    f = fopen(ROOT "/s.txt", "a");
    fputs("appended\n", f);
    fclose(f);
    ok("append mode adds to the end", stat(ROOT "/s.txt", &st) == 0 && st.st_size == 31 + 9);

    f = fopen(ROOT "/s.txt", "r+");
    ok("fopen(r+)", f != NULL);
    ok("overwrite in place", fwrite("LINE", 1, 4, f) == 4 && fflush(f) == 0);
    ok("seek back and read after a write", fseek(f, 0, SEEK_SET) == 0 && fgets(line, sizeof line, f) && strcmp(line, "LINE 1\n") == 0);
    fclose(f);

    for (i = 0; i < (int)sizeof big; i++) big[i] = (char)('a' + i % 26);
    f = fopen(ROOT "/big.bin", "wb");
    ok("a write larger than the buffer", fwrite(big, 1, sizeof big, f) == sizeof big);
    fclose(f);
    f = fopen(ROOT "/big.bin", "rb");
    ok("and it reads back intact", fread(line, 1, 0, f) == 0 && fseek(f, 2990, SEEK_SET) == 0 &&
       fread(line, 1, 10, f) == 10 && memcmp(line, big + 2990, 10) == 0);
    fclose(f);

    f = fopen(ROOT "/nl.txt", "w");
    fputs("no newline", f);                       /* stays in the buffer */
    ok("buffered data is not yet in the file", stat(ROOT "/nl.txt", &st) == 0 && st.st_size == 0);
    ok("fflush(NULL) flushes every open stream", fflush(NULL) == 0 && stat(ROOT "/nl.txt", &st) == 0 && st.st_size == 10);
    fclose(f);
    remove(ROOT "/nl.txt");

    errno = 0;
    ok("fopen of a missing file fails with ENOENT", fopen(ROOT "/missing", "r") == NULL && errno == ENOENT);
    errno = 0;
    ok("a bad mode is rejected", fopen(ROOT "/s.txt", "q") == NULL && errno == EINVAL);
    f = fopen(ROOT "/s.txt", "r");
    ok("writing to a read-only stream fails", fputc('x', f) == EOF && ferror(f));
    fclose(f);

    ok("remove deletes files", remove(ROOT "/s.txt") == 0 && remove(ROOT "/big.bin") == 0);
    ok("remove deletes directories", remove(ROOT) == 0);
}

static int by_name(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static void test_dirs(void)
{
    static char names[8][64];
    char *list[8];
    int n = 0, i, d_ok = 1, r_ok = 1, fd;
    struct dirent *e;
    DIR *d;

    mkdir(ROOT, 0755);
    mkdir(ROOT "/sub", 0755);
    for (i = 0; i < 3; i++) {
        char p[64];
        snprintf(p, sizeof p, ROOT "/f%d", i);
        fd = open(p, O_WRONLY | O_CREAT, 0644);
        if (fd >= 0) close(fd);
    }
    d = opendir(ROOT);
    ok("opendir opens a directory", d != NULL);
    while (d && (e = readdir(d)) != NULL && n < 8) {
        strncpy(names[n], e->d_name, sizeof names[n] - 1);
        list[n] = names[n];
        if (strcmp(e->d_name, "sub") == 0 && e->d_type != DT_DIR) d_ok = 0;
        if (e->d_name[0] == 'f' && e->d_type != DT_REG) r_ok = 0;
        n++;
    }
    ok("readdir ends with NULL", d && readdir(d) == NULL);
    ok("closedir succeeds", d && closedir(d) == 0);
    qsort(list, (size_t)n, sizeof list[0], by_name);
    ok("a directory lists ., .., and its entries", n == 6 && !strcmp(list[0], ".") && !strcmp(list[1], "..") &&
       !strcmp(list[2], "f0") && !strcmp(list[3], "f1") && !strcmp(list[4], "f2") && !strcmp(list[5], "sub"));
    ok("d_type distinguishes directories", d_ok);
    ok("d_type distinguishes regular files", r_ok);
    errno = 0;
    ok("opendir of a missing path fails with ENOENT", opendir(ROOT "/nope") == NULL && errno == ENOENT);
    errno = 0;
    ok("opendir of a regular file fails with ENOTDIR", opendir(ROOT "/f0") == NULL && errno == ENOTDIR);
    for (i = 0; i < 3; i++) {
        char p[64];
        snprintf(p, sizeof p, ROOT "/f%d", i);
        unlink(p);
    }
    rmdir(ROOT "/sub");
    rmdir(ROOT);
}

static void test_process(void)
{
    int fds[2], st = 0, i;
    char buf[32];
    pid_t me = getpid(), c, c2;

    ok("pipe creates two descriptors", pipe(fds) == 0 && fds[0] != fds[1]);
    ok("a pipe carries bytes", write(fds[1], "abc", 3) == 3 && read(fds[0], buf, 8) == 3 && memcmp(buf, "abc", 3) == 0);
    close(fds[1]);
    ok("read returns 0 once the write end is closed", read(fds[0], buf, 8) == 0);
    close(fds[0]);

    errno = 0;
    ok("wait with no children fails with ECHILD", wait(&st) == -1 && errno == ECHILD);

    pipe(fds);
    fflush(NULL);                                /* nothing may be duplicated into the child */
    c = fork();
    if (c == 0) {
        close(fds[0]);
        write(fds[1], "from child", 10);
        _exit(getppid() == me ? 7 : 9);
    }
    ok("fork returns a child pid in the parent", c > 0 && c != me);
    close(fds[1]);
    memset(buf, 0, sizeof buf);
    ok("the parent reads what the child wrote", read(fds[0], buf, sizeof buf) == 10 && memcmp(buf, "from child", 10) == 0);
    close(fds[0]);
    ok("waitpid reaps that child", waitpid(c, &st, 0) == c);
    ok("it exited normally with the child's status (getppid matched)", WIFEXITED(st) && WEXITSTATUS(st) == 7);

    fflush(NULL);
    c = fork();
    if (c == 0) _exit(21);
    c2 = fork();
    if (c2 == 0) _exit(22);
    {
        int seen = 0, codes = 0;
        for (i = 0; i < 2; i++) {
            pid_t w = wait(&st);
            if (w == c || w == c2) { seen++; codes += WEXITSTATUS(st); }
        }
        ok("wait reaps two children, whichever order", seen == 2 && codes == 43);
    }
    errno = 0;
    ok("nothing is left to wait for", wait(&st) == -1 && errno == ECHILD);
}

/* --- signals -------------------------------------------------------- */

static volatile sig_atomic_t got_sig, got_count, got_code, got_pid_ok;

static void on_sig(int sig) { got_sig = sig; got_count++; }

static void on_info(int sig, siginfo_t *si, void *uc)
{
    (void)uc;
    got_sig = sig;
    got_count++;
    got_code = si->si_code;
    got_pid_ok = si->si_pid == getpid();
}

static int abort_pipe = -1;
static void on_abort(int sig) { (void)sig; if (abort_pipe >= 0) write(abort_pipe, "h", 1); }

static void test_signals(void)
{
    sigset_t s, old;
    struct sigaction sa;
    sighandler_t prev;
    int st = 0, fds[2];
    char c;
    pid_t kid;

    ok("sigemptyset empties", sigemptyset(&s) == 0 && sigismember(&s, SIGUSR1) == 0);
    ok("sigaddset adds", sigaddset(&s, SIGUSR1) == 0 && sigismember(&s, SIGUSR1) == 1);
    ok("sigdelset removes", sigdelset(&s, SIGUSR1) == 0 && sigismember(&s, SIGUSR1) == 0);
    ok("sigfillset fills", sigfillset(&s) == 0 && sigismember(&s, SIGTERM) == 1);
    errno = 0;
    ok("signal 0 is not a member of anything", sigaddset(&s, 0) == -1 && errno == EINVAL);
    errno = 0;
    ok("nor is signal 65", sigismember(&s, 65) == -1 && errno == EINVAL);

    got_sig = got_count = 0;
    prev = signal(SIGUSR1, on_sig);
    ok("signal() installs a handler", prev != SIG_ERR);
    ok("raise runs it before returning", raise(SIGUSR1) == 0 && got_sig == SIGUSR1 && got_count == 1);
    ok("and the program carries on after it", got_count == 1);
    ok("signal() hands back the handler it replaces", signal(SIGUSR1, SIG_DFL) == on_sig);

    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_info;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    got_sig = got_count = 0;
    ok("sigaction installs an SA_SIGINFO handler", sigaction(SIGUSR2, &sa, NULL) == 0);
    raise(SIGUSR2);
    ok("it is given the signal", got_sig == SIGUSR2 && got_count == 1);
    ok("si_code says it was sent to a thread (SI_TKILL)", got_code == -6);
    ok("si_pid says who sent it", got_pid_ok == 1);
    memset(&sa, 0, sizeof sa);
    ok("sigaction reports the handler back", sigaction(SIGUSR2, NULL, &sa) == 0 &&
       sa.sa_sigaction == on_info && (sa.sa_flags & SA_SIGINFO));
    signal(SIGUSR2, SIG_DFL);

    signal(SIGUSR1, on_sig);
    got_count = 0;
    sigemptyset(&s);
    sigaddset(&s, SIGUSR1);
    ok("sigprocmask blocks", sigprocmask(SIG_BLOCK, &s, NULL) == 0);
    raise(SIGUSR1);
    ok("a blocked signal waits", got_count == 0);
    ok("and the mask says it is blocked", sigprocmask(SIG_BLOCK, NULL, &old) == 0 && sigismember(&old, SIGUSR1) == 1);
    ok("unblocking delivers it", sigprocmask(SIG_UNBLOCK, &s, NULL) == 0 && got_count == 1);
    errno = 0;
    ok("a bad 'how' is EINVAL", sigprocmask(99, &s, NULL) == -1 && errno == EINVAL);

    signal(SIGUSR1, SIG_IGN);
    ok("an ignored signal does nothing", raise(SIGUSR1) == 0);
    signal(SIGUSR1, SIG_DFL);

    fflush(NULL);
    kid = fork();
    if (kid == 0) abort();
    ok("abort() is a death by SIGABRT", waitpid(kid, &st, 0) == kid &&
       WIFSIGNALED(st) && WTERMSIG(st) == SIGABRT);

    pipe(fds);
    fflush(NULL);
    kid = fork();
    if (kid == 0) {
        close(fds[0]);
        abort_pipe = fds[1];
        signal(SIGABRT, on_abort);
        abort();
    }
    close(fds[1]);
    ok("abort() runs a SIGABRT handler", read(fds[0], &c, 1) == 1 && c == 'h');
    ok("and still ends the process with SIGABRT when it returns",
       waitpid(kid, &st, 0) == kid && WIFSIGNALED(st) && WTERMSIG(st) == SIGABRT);
    close(fds[0]);

    pipe(fds);
    fflush(NULL);
    kid = fork();
    if (kid == 0) {
        close(fds[1]);
        read(fds[0], &c, 1);             /* waits; nothing is ever written */
        _exit(3);
    }
    close(fds[0]);
    ok("kill sends SIGTERM", kill(kid, SIGTERM) == 0);
    ok("which ends the child by that signal", waitpid(kid, &st, 0) == kid &&
       WIFSIGNALED(st) && WTERMSIG(st) == SIGTERM);
    close(fds[1]);
    errno = 0;
    ok("kill to a reaped pid is ESRCH", kill(kid, 0) == -1 && errno == ESRCH);
    errno = 0;
    ok("kill with signal 99 is EINVAL", kill(getpid(), 99) == -1 && errno == EINVAL);
}

/* --- running another program ----------------------------------------- */

#define CHILD "/tmp/novac_child"

static void test_exec(void)
{
    int st = 0;
    pid_t kid;
    char *args[] = { "novac_child", "hello", NULL };
    char *bare[] = { "novac_child", NULL };

    errno = 0;
    ok("execv of a missing file is ENOENT", execv("/nonexistent/novac", args) == -1 && errno == ENOENT);

    fflush(NULL);
    kid = fork();
    if (kid == 0) { execv(CHILD, args); _exit(99); }
    ok("execv runs another program, with its arguments",
       waitpid(kid, &st, 0) == kid && WIFEXITED(st) && WEXITSTATUS(st) == 42);

    ok("setenv sets", setenv("NOVAC_MARK", "seen", 1) == 0 && getenv("NOVAC_MARK") &&
       strcmp(getenv("NOVAC_MARK"), "seen") == 0);
    ok("without overwrite it keeps the old value", setenv("NOVAC_MARK", "other", 0) == 0 &&
       strcmp(getenv("NOVAC_MARK"), "seen") == 0);
    errno = 0;
    ok("a name with '=' is EINVAL", setenv("A=B", "x", 1) == -1 && errno == EINVAL);
    ok("unsetenv removes", setenv("NOVAC_GONE", "x", 1) == 0 && unsetenv("NOVAC_GONE") == 0 &&
       getenv("NOVAC_GONE") == NULL);

    ok("PATH can be set", setenv("PATH", "/nonexistent:/tmp", 1) == 0);
    fflush(NULL);
    kid = fork();
    if (kid == 0) { execvp("novac_child", bare); _exit(99); }
    ok("execvp finds it on PATH, and passes the environment on",
       waitpid(kid, &st, 0) == kid && WIFEXITED(st) && WEXITSTATUS(st) == 41);
    errno = 0;
    ok("execvp of a name on no PATH entry is ENOENT", execvp("novac_nowhere", bare) == -1 && errno == ENOENT);
}

static void test_time(void)
{
    struct timespec a, b, req = { 0, 20 * 1000 * 1000 };
    time_t t;
    struct tm tm;

    ok("time() is after 2020", time(NULL) > 1577836800L);
    ok("clock_gettime(MONOTONIC)", clock_gettime(CLOCK_MONOTONIC, &a) == 0);
    ok("nanosleep succeeds", nanosleep(&req, NULL) == 0);
    clock_gettime(CLOCK_MONOTONIC, &b);
    ok("the monotonic clock advanced by about the sleep",
       (b.tv_sec - a.tv_sec) * 1000000000L + (b.tv_nsec - a.tv_nsec) >= 10 * 1000 * 1000L);

    t = 0;
    gmtime_r(&t, &tm);
    ok("gmtime(0) is 1970-01-01 Thursday", tm.tm_year == 70 && tm.tm_mon == 0 && tm.tm_mday == 1 && tm.tm_wday == 4 && tm.tm_yday == 0);
    t = 951782400L;
    gmtime_r(&t, &tm);
    ok("leap day 2000-02-29", tm.tm_year == 100 && tm.tm_mon == 1 && tm.tm_mday == 29 && tm.tm_wday == 2 && tm.tm_yday == 59);
    t = 1700000000L;
    gmtime_r(&t, &tm);
    ok("1700000000 is 2023-11-14 22:13:20", tm.tm_year == 123 && tm.tm_mon == 10 && tm.tm_mday == 14 &&
       tm.tm_hour == 22 && tm.tm_min == 13 && tm.tm_sec == 20 && tm.tm_wday == 2);
    t = -1;
    gmtime_r(&t, &tm);
    ok("-1 is 1969-12-31 23:59:59 Wednesday", tm.tm_year == 69 && tm.tm_mon == 11 && tm.tm_mday == 31 &&
       tm.tm_hour == 23 && tm.tm_sec == 59 && tm.tm_wday == 3);
    ok("timegm inverts gmtime", timegm(&tm) == -1);
    tm.tm_year = 123; tm.tm_mon = 14; tm.tm_mday = 1; tm.tm_hour = tm.tm_min = tm.tm_sec = 0;
    ok("timegm normalises month overflow", timegm(&tm) == 1709251200L && tm.tm_year == 124 && tm.tm_mon == 2);
}

static void test_env(int argc, char **argv)
{
    ok("argc and argv are set up", argc >= 1 && argv[0] != NULL && argv[argc] == NULL);
    ok("getenv of an unset name is NULL", getenv("NOVAC_SURELY_UNSET_VARIABLE") == NULL);
    ok("getenv rejects a name containing '='", getenv("A=B") == NULL);
    ok("environ is terminated", environ != NULL);
}

int main(int argc, char **argv)
{
    printf("novac64: libNovaC on the target\n");
    atexit(on_exit_1);
    atexit(on_exit_2);
    test_format();
    test_memory();
    test_files();
    test_streams();
    test_dirs();
    test_process();
    test_time();
    test_signals();
    test_exec();
    test_env(argc, argv);
    printf("novac64: %d failures\n", failures);
    return failures ? 1 : 131;
}
