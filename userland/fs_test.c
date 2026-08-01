/* fs_test.c - a Linux filesystem program, run on Novaris.
 *
 * Same rules as every other test here: raw `int $0x80`, Linux syscall
 * numbers, `gcc -m32 -static -nostdlib -ffreestanding`, linked against
 * nothing. The same binary runs on the Linux build host and on Novaris
 * and the transcripts are compared.
 *
 * Everything happens under /tmp/novaris_fstest, a directory this program
 * creates and removes, so the host run touches nothing it does not own.
 *
 * File semantics are unusually good material for a comparison test,
 * because they are *precise*: rmdir on a non-empty directory is
 * ENOTEMPTY and not ENOENT, unlink on a directory is EISDIR and not
 * EPERM, O_APPEND re-evaluates the end on every write rather than seeking
 * once. Each of those is a specific number the two systems either agree
 * on or do not.
 *
 * See ROADMAP.md Milestone 26.
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

#define SYS_exit         1
#define SYS_read         3
#define SYS_write        4
#define SYS_open         5
#define SYS_close        6
#define SYS_unlink      10
#define SYS_lseek       19
#define SYS_access      33
#define SYS_rename      38
#define SYS_mkdir       39
#define SYS_rmdir       40
#define SYS_stat64     195
#define SYS_fstat64    197
#define SYS_ftruncate64 194
#define SYS_getdents64 220

#define O_RDONLY   0x0000
#define O_WRONLY   0x0001
#define O_RDWR     0x0002
#define O_CREAT    0x0040
#define O_EXCL     0x0080
#define O_TRUNC    0x0200
#define O_APPEND   0x0400

#define SEEK_SET 0
#define SEEK_END 2

#define ENOENT     -2
#define EEXIST    -17
#define EISDIR    -21
#define ENOTDIR   -20
#define ENOTEMPTY -39

#define S_IFMT  0170000
#define S_IFREG 0100000
#define S_IFDIR 0040000

/* --- a few bytes of libc ------------------------------------------------- */

static unsigned slen(const char* s) { unsigned n = 0; while (s[n]) n++; return n; }
static void out(const char* s) { sc3(SYS_write, 1, (long)s, slen(s)); }
static void check(const char* label, int ok) {
    out(ok ? "  [ok]   " : "  [FAIL] ");
    out(label);
    out("\n");
}
static int streq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}
static int memeq(const char* a, const char* b, unsigned n) {
    for (unsigned i = 0; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}

/* --- paths --------------------------------------------------------------- */

#define DIR   "/tmp/novaris_fstest"
#define SUB   DIR "/sub"
#define FILE1 DIR "/one.txt"
#define FILE2 DIR "/two.txt"

static long stat_mode(const char* path, long* size_out) {
    unsigned char st[144];
    long r = sc2(SYS_stat64, (long)path, (long)st);
    if (r < 0) return r;
    if (size_out) *size_out = *(long*)(st + 44);
    return *(unsigned*)(st + 16);
}

/* Removes everything this test creates, so a run that died partway does
 * not poison the next one - and so the host is left exactly as found. */
static void scrub(void) {
    sc1(SYS_unlink, (long)FILE1);
    sc1(SYS_unlink, (long)FILE2);
    sc1(SYS_unlink, (long)(SUB "/inner.txt"));
    sc1(SYS_unlink, (long)(DIR "/hello.txt"));
    sc1(SYS_rmdir, (long)SUB);
    sc1(SYS_rmdir, (long)DIR);
}

int main_(void) {
    out("filesystem test - written for Linux, running unmodified\n\n");
    scrub();

    /* 1. Directories. */
    out("1. mkdir and rmdir\n");
    check("mkdir succeeds", sc2(SYS_mkdir, (long)DIR, 0755) == 0);
    check("mkdir again is EEXIST", sc2(SYS_mkdir, (long)DIR, 0755) == EEXIST);
    check("the new directory stats as a directory",
          (stat_mode(DIR, 0) & S_IFMT) == S_IFDIR);
    check("mkdir inside a path that does not exist is ENOENT",
          sc2(SYS_mkdir, (long)"/tmp/novaris_fstest/no/such/dir", 0755) == ENOENT);

    /* 2. Creating, writing and reading back. */
    out("\n2. create, write, read back\n");
    const char* msg = "hello, filesystem\n";
    long n = (long)slen(msg);
    {
        long fd = sc3(SYS_open, (long)FILE1, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        check("open(O_CREAT|O_WRONLY) succeeds", fd >= 0);
        check("write returns the byte count",
              sc3(SYS_write, fd, (long)msg, n) == n);
        sc1(SYS_close, fd);
    }
    {
        long size = -1;
        check("it stats as a regular file",
              (stat_mode(FILE1, &size) & S_IFMT) == S_IFREG);
        check("with the size that was written", size == n);
    }
    {
        char buf[64];
        long fd = sc3(SYS_open, (long)FILE1, O_RDONLY, 0);
        check("open(O_RDONLY) succeeds", fd >= 0);
        long got = sc3(SYS_read, fd, (long)buf, 64);
        check("read returns the byte count", got == n);
        check("and the bytes come back unchanged", memeq(buf, msg, (unsigned)n));
        check("a second read at EOF returns 0",
              sc3(SYS_read, fd, (long)buf, 64) == 0);
        sc1(SYS_close, fd);
    }

    /* 3. O_APPEND, which is not "seek to the end once". */
    out("\n3. append\n");
    {
        const char* more = "second line\n";
        long m = (long)slen(more);
        long fd = sc3(SYS_open, (long)FILE1, O_WRONLY | O_APPEND, 0);
        check("open(O_APPEND) succeeds", fd >= 0);
        sc3(SYS_write, fd, (long)more, m);
        /* Seeking back to the start must not make the next write land
         * there: O_APPEND re-evaluates the end every time. */
        sc3(SYS_lseek, fd, 0, SEEK_SET);
        sc3(SYS_write, fd, (long)more, m);
        sc1(SYS_close, fd);

        long size = -1;
        stat_mode(FILE1, &size);
        check("both appends went to the end", size == n + 2 * m);

        char buf[128];
        fd = sc3(SYS_open, (long)FILE1, O_RDONLY, 0);
        sc3(SYS_read, fd, (long)buf, 128);
        sc1(SYS_close, fd);
        check("the original first line is still first",
              memeq(buf, msg, (unsigned)n));
    }

    /* 4. Seeking and positioned reads. */
    out("\n4. lseek\n");
    {
        char buf[32];
        long fd = sc3(SYS_open, (long)FILE1, O_RDONLY, 0);
        check("lseek to 7 reports 7", sc3(SYS_lseek, fd, 7, SEEK_SET) == 7);
        sc3(SYS_read, fd, (long)buf, 10);
        check("and reads from there", memeq(buf, "filesystem", 10));
        check("lseek to the end reports the size",
              sc3(SYS_lseek, fd, 0, SEEK_END) == n + 2 * (long)slen("second line\n"));
        sc1(SYS_close, fd);
    }

    /* 5. Truncation, both kinds. */
    out("\n5. truncate\n");
    {
        long fd = sc3(SYS_open, (long)FILE1, O_WRONLY, 0);
        check("ftruncate to 5 succeeds", sc3(SYS_ftruncate64, fd, 5, 0) == 0);
        sc1(SYS_close, fd);
        long size = -1;
        stat_mode(FILE1, &size);
        check("the file is now 5 bytes", size == 5);

        fd = sc3(SYS_open, (long)FILE1, O_WRONLY | O_TRUNC, 0);
        sc1(SYS_close, fd);
        stat_mode(FILE1, &size);
        check("O_TRUNC empties it", size == 0);
    }

    /* 6. Rename, which is how every program updates a file safely. */
    out("\n6. rename\n");
    {
        long fd = sc3(SYS_open, (long)FILE1, O_WRONLY | O_TRUNC, 0);
        sc3(SYS_write, fd, (long)msg, n);
        sc1(SYS_close, fd);

        check("rename succeeds", sc2(SYS_rename, (long)FILE1, (long)FILE2) == 0);
        check("the old name is gone",
              sc3(SYS_open, (long)FILE1, O_RDONLY, 0) == ENOENT);
        long size = -1;
        check("the new name is a file of the right size",
              (stat_mode(FILE2, &size) & S_IFMT) == S_IFREG && size == n);
        check("rename back succeeds",
              sc2(SYS_rename, (long)FILE2, (long)FILE1) == 0);
    }

    /* 7. Reading a directory. */
    out("\n7. getdents64\n");
    {
        check("mkdir a subdirectory", sc2(SYS_mkdir, (long)SUB, 0755) == 0);
        long fd = sc3(SYS_open, (long)(SUB "/inner.txt"),
                      O_WRONLY | O_CREAT, 0644);
        sc3(SYS_write, fd, (long)"x", 1);
        sc1(SYS_close, fd);

        char buf[1024];
        fd = sc3(SYS_open, (long)DIR, O_RDONLY, 0);
        check("a directory can be opened read-only", fd >= 0);
        long got = sc3(SYS_getdents64, fd, (long)buf, 1024);
        check("getdents64 returns bytes", got > 0);
        sc1(SYS_close, fd);

        /* Collect the names, then report them sorted - the order a
         * directory hands them back is nobody's contract. */
        const char* names[16];
        int count = 0;
        for (long off = 0; off < got && count < 16; ) {
            unsigned short reclen = *(unsigned short*)(buf + off + 16);
            names[count++] = buf + off + 19;
            if (!reclen) break;
            off += reclen;
        }
        for (int i = 1; i < count; i++) {
            const char* k = names[i];
            int j = i - 1;
            while (j >= 0) {
                const char* a = names[j]; const char* b = k;
                while (*a && *a == *b) { a++; b++; }
                if ((unsigned char)*a <= (unsigned char)*b) break;
                names[j + 1] = names[j];
                j--;
            }
            names[j + 1] = k;
        }
        out("  entries:");
        for (int i = 0; i < count; i++) { out(" "); out(names[i]); }
        out("\n");

        int has_dot = 0, has_dotdot = 0, has_one = 0, has_sub = 0;
        for (int i = 0; i < count; i++) {
            if (streq(names[i], ".")) has_dot = 1;
            if (streq(names[i], "..")) has_dotdot = 1;
            if (streq(names[i], "one.txt")) has_one = 1;
            if (streq(names[i], "sub")) has_sub = 1;
        }
        check("it lists . and ..", has_dot && has_dotdot);
        check("and both things that were created", has_one && has_sub);
        check("and nothing else", count == 4);
    }

    /* 8. The errors, which are the part a program actually depends on. */
    out("\n8. errors\n");
    check("rmdir on a non-empty directory is ENOTEMPTY",
          sc1(SYS_rmdir, (long)SUB) == ENOTEMPTY);
    check("unlink on a directory is EISDIR",
          sc1(SYS_unlink, (long)SUB) == EISDIR);
    check("opening a directory for writing is EISDIR",
          sc3(SYS_open, (long)SUB, O_WRONLY, 0) == EISDIR);
    check("O_CREAT|O_EXCL on something that exists is EEXIST",
          sc3(SYS_open, (long)FILE1, O_WRONLY | O_CREAT | O_EXCL, 0644) == EEXIST);
    check("a path through a file rather than a directory fails",
          sc3(SYS_open, (long)(FILE1 "/x"), O_RDONLY, 0) < 0);
    check("opening what is not there is ENOENT",
          sc3(SYS_open, (long)(DIR "/absent"), O_RDONLY, 0) == ENOENT);
    /* "hello.txt" and "readme.txt" both exist at the root on Novaris and
     * in the host's working directory, and neither exists here. A miss
     * inside a directory that *does* exist must stay a miss - if a
     * kernel resolved paths by last component alone, or fell back to
     * doing so unconditionally, this would find the wrong file and
     * report success. */
    check("a name that exists elsewhere does not shadow a miss here",
          sc3(SYS_open, (long)(DIR "/hello.txt"), O_RDONLY, 0) == ENOENT &&
          sc3(SYS_open, (long)(DIR "/readme.txt"), O_RDONLY, 0) == ENOENT);

    /* 9. Removing it all again. */
    out("\n9. unlink and rmdir\n");
    check("unlink the inner file",
          sc1(SYS_unlink, (long)(SUB "/inner.txt")) == 0);
    check("rmdir the now-empty subdirectory", sc1(SYS_rmdir, (long)SUB) == 0);
    check("the subdirectory is gone",
          sc3(SYS_open, (long)SUB, O_RDONLY, 0) == ENOENT);
    check("unlink the file", sc1(SYS_unlink, (long)FILE1) == 0);
    check("the file is gone",
          sc2(SYS_access, (long)FILE1, 0) == ENOENT);
    check("rmdir the test directory", sc1(SYS_rmdir, (long)DIR) == 0);
    check("and it is gone too", sc2(SYS_access, (long)DIR, 0) == ENOENT);

    out("\nfilesystem test done.\n");
    return 0;
}

void _start(void) {
    sc1(SYS_exit, main_());
    __builtin_unreachable();
}
