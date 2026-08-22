/* cwd64.c - the working directory and symbolic links, through real glibc.
 *
 * Milestone 68. Before it, this kernel answered every relative path with
 * -ENOENT and had no symlinks at all, which is why this program is worth
 * having: almost everything it does was impossible one commit ago, and
 * all of it is ordinary.
 *
 * It is deliberately not a list of syscalls. It is what Wine does to a
 * prefix - chdir into a directory it just made, name files relative to
 * where it is, reach drive C through a symlink to "../drive_c", and tell
 * a link from the thing it points at - written the way a normal program
 * would write it, through glibc rather than by hand. glibc chooses the
 * calls (openat with AT_FDCWD, unlinkat with AT_REMOVEDIR, newfstatat)
 * and this kernel has to answer whichever it picks.
 *
 * Every line of output has to be byte-identical to the host's, so
 * nothing here prints an address, a pid, or the initial working
 * directory - the one thing that legitimately differs between the two.
 * The program chdirs to /tmp before it does anything, so both runs start
 * from the same place.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>

#define ROOT "/tmp/novaris_cwd_test"

static int failures;

static void ok(const char *what, int cond)
{
    printf("%-4s %s\n", cond ? "ok" : "FAIL", what);
    if (!cond)
        failures++;
}

/* getcwd into a static buffer, so a failure prints as a comparable
 * string rather than as a segfault. */
static const char *here(void)
{
    static char buf[1024];

    if (!getcwd(buf, sizeof buf))
        strcpy(buf, "(getcwd failed)");
    return buf;
}

static void cleanup(void)
{
    /* Deepest first. Nothing here recurses: the tree is known. */
    unlink(ROOT "/drive_c/windows/system32/ntdll.dll");
    unlink(ROOT "/dosdevices/c:");
    unlink(ROOT "/dangling");
    unlink(ROOT "/system32");
    rmdir(ROOT "/drive_c/windows/system32");
    rmdir(ROOT "/drive_c/windows");
    rmdir(ROOT "/drive_c");
    rmdir(ROOT "/dosdevices");
    rmdir(ROOT);
}

int main(void)
{
    char buf[1024];
    struct stat st, lst;
    ssize_t n;
    int fd;

    /* A run must not depend on what a previous run left behind - on the
     * host this file survives, on the guest the filesystem is new every
     * boot, and the two have to agree. */
    cleanup();

    if (chdir("/tmp") != 0) {
        printf("FAIL cannot chdir to /tmp\n");
        return 1;
    }

    /* --- the working directory itself ---------------------------- */

    ok("chdir to an absolute path", strcmp(here(), "/tmp") == 0);

    ok("mkdir a prefix", mkdir(ROOT, 0755) == 0);

    /* Relative, from /tmp. This is the call that used to be -ENOENT. */
    ok("chdir to a relative path", chdir("novaris_cwd_test") == 0);
    ok("and getcwd says where that was", strcmp(here(), ROOT) == 0);

    /* A path built out of the tree rather than out of text: ".." and "."
     * are resolved by walking, so getcwd answers with where the process
     * landed and not with how it got there. */
    ok("chdir through . and ..", chdir("./../novaris_cwd_test/.") == 0);
    ok("and getcwd normalises it", strcmp(here(), ROOT) == 0);

    ok("mkdir relative", mkdir("drive_c", 0755) == 0);
    ok("mkdir nested relative", mkdir("drive_c/windows", 0755) == 0);
    ok("mkdir deeper", mkdir("drive_c/windows/system32", 0755) == 0);
    ok("mkdir a sibling", mkdir("dosdevices", 0755) == 0);

    /* --- a file, named relative to where we are ------------------- */

    fd = open("drive_c/windows/system32/ntdll.dll",
              O_CREAT | O_WRONLY | O_TRUNC, 0644);
    ok("create a file by relative path", fd >= 0);
    if (fd >= 0) {
        ok("write to it", write(fd, "MZ\x90\x00", 4) == 4);
        close(fd);
    }

    /* The same file, by an absolute path. If the working directory were
     * being ignored these would be two different files, or one of them
     * would not exist. */
    ok("stat it by absolute path",
       stat(ROOT "/drive_c/windows/system32/ntdll.dll", &st) == 0);
    ok("and it is 4 bytes", st.st_size == 4);

    /* --- symlinks, which is how a prefix reaches drive C ---------- */

    ok("symlink with a relative target",
       symlink("../drive_c", "dosdevices/c:") == 0);

    n = readlink("dosdevices/c:", buf, sizeof buf - 1);
    if (n < 0)
        n = 0;
    buf[n] = 0;
    ok("readlink gives the target back verbatim",
       strcmp(buf, "../drive_c") == 0);

    /* Through the link, and the target is relative to the directory the
     * link sits in - not to the working directory. Getting that wrong is
     * the classic symlink bug and it would resolve to /tmp/drive_c. */
    ok("open a file through the link",
       (fd = open("dosdevices/c:/windows/system32/ntdll.dll", O_RDONLY)) >= 0);
    if (fd >= 0) {
        memset(buf, 0, sizeof buf);
        ok("and it reads back what was written",
           read(fd, buf, 4) == 4 && memcmp(buf, "MZ\x90\x00", 4) == 0);
        close(fd);
    }

    /* stat follows, lstat does not. A tree walker that could not tell
     * these apart would descend into the target instead of copying the
     * link. */
    ok("stat through a link finds a directory",
       stat("dosdevices/c:", &st) == 0 && S_ISDIR(st.st_mode));
    ok("lstat on the same path finds a link",
       lstat("dosdevices/c:", &lst) == 0 && S_ISLNK(lst.st_mode));
    ok("and they are not the same object", st.st_ino != lst.st_ino);

    /* chdir through a link lands on the target, and getcwd says so -
     * the path the process got there by is not the path it is at. */
    ok("chdir through a link", chdir("dosdevices/c:/windows") == 0);
    ok("and getcwd reports where it landed",
       strcmp(here(), ROOT "/drive_c/windows") == 0);
    ok("chdir back", chdir(ROOT) == 0);

    /* An absolute symlink, and one that points nowhere. Both are legal;
     * a filesystem that validated targets would reject the second, and
     * every real one accepts it. */
    ok("symlink with an absolute target",
       symlink(ROOT "/drive_c/windows/system32", "system32") == 0);
    ok("open through the absolute link",
       (fd = open("system32/ntdll.dll", O_RDONLY)) >= 0);
    if (fd >= 0)
        close(fd);

    ok("a dangling symlink is created", symlink("nowhere", "dangling") == 0);
    ok("opening it fails with ENOENT",
       open("dangling", O_RDONLY) < 0 && errno == ENOENT);
    ok("but lstat still sees the link", lstat("dangling", &lst) == 0);
    n = readlink("dangling", buf, sizeof buf - 1);
    if (n < 0)
        n = 0;
    buf[n] = 0;
    ok("and readlink still answers", strcmp(buf, "nowhere") == 0);

    /* readlink on something that is not a link is EINVAL, which is how a
     * caller tells "nothing here" from "not a link". */
    ok("readlink on a regular file is EINVAL",
       readlink("drive_c/windows/system32/ntdll.dll", buf, sizeof buf) < 0
       && errno == EINVAL);

    /* Removing a link removes the link. If unlink followed it, the file
     * underneath would disappear instead - silently, and only noticed
     * much later. */
    ok("unlink a symlink", unlink("dosdevices/c:") == 0);
    ok("and the target survives it",
       stat("drive_c/windows/system32/ntdll.dll", &st) == 0);

    /* --- rename, which is how a file is replaced (Milestone 77) ---- */
    /* Not a tidy-up of this test's subject but the same subject: a
     * rename moves a node between directories, which is the child-list
     * bookkeeping every assertion above depends on, seen from the one
     * direction that changes it after the fact.
     *
     * The wineserver saves its registry this way - write a temporary,
     * then move it into place - so that nobody ever reads a half-written
     * registry. Without it wineboot writes reg30000.tmp and starts
     * again, forever. */
    {
        int fd;
        char rbuf[16];

        fd = open("tmpfile", O_RDWR | O_CREAT | O_TRUNC, 0644);
        ok("a temporary file", fd >= 0);
        ok("with contents", write(fd, "final", 5) == 5);
        close(fd);

        ok("rename moves it", rename("tmpfile", "realfile") == 0);
        ok("the old name is gone", stat("tmpfile", &st) < 0);
        ok("the new name is there", stat("realfile", &st) == 0);

        fd = open("realfile", O_RDONLY);
        memset(rbuf, 0, sizeof(rbuf));
        ok("and the contents came with it",
           read(fd, rbuf, sizeof(rbuf)) == 5 && memcmp(rbuf, "final", 5) == 0);
        close(fd);

        /* Replacing an existing file is the case that matters: this is
         * what "install the new registry over the old one" is. */
        fd = open("tmpfile", O_RDWR | O_CREAT | O_TRUNC, 0644);
        write(fd, "second", 6);
        close(fd);
        ok("rename over an existing file replaces it",
           rename("tmpfile", "realfile") == 0);
        fd = open("realfile", O_RDONLY);
        memset(rbuf, 0, sizeof(rbuf));
        ok("and the replacement is what is there",
           read(fd, rbuf, sizeof(rbuf)) == 6 && memcmp(rbuf, "second", 6) == 0);
        close(fd);

        /* Into another directory, which is the part that exercises the
         * move rather than the rename. */
        ok("rename into another directory",
           rename("realfile", "drive_c/moved") == 0);
        ok("it is not where it was", stat("realfile", &st) < 0);
        ok("and it is where it went", stat("drive_c/moved", &st) == 0);
        ok("with its size intact", st.st_size == 6);

        ok("renaming something that is not there is ENOENT",
           rename("no_such_file", "anywhere") < 0 && errno == ENOENT);
        ok("a directory cannot be renamed into itself",
           rename("drive_c", "drive_c/inside") < 0);

        ok("and it can be removed again", unlink("drive_c/moved") == 0);
    }

    /* --- ftruncate, both directions -------------------------------- */
    {
        int fd = open("trunc", O_RDWR | O_CREAT | O_TRUNC, 0644);
        char tbuf[16];

        ok("a file to resize", fd >= 0);
        ok("with eight bytes", write(fd, "abcdefgh", 8) == 8);

        ok("shrinking reports success", ftruncate(fd, 3) == 0);
        ok("and the file is shorter", stat("trunc", &st) == 0 && st.st_size == 3);

        /* Growing has to expose zeros, not whatever the allocator had.
         * This heap hands back memory that was somebody else's file a
         * moment ago, so "it happened to be zero" is not something to
         * rely on. */
        ok("growing reports success", ftruncate(fd, 8) == 0);
        ok("and the file is longer again",
           stat("trunc", &st) == 0 && st.st_size == 8);
        lseek(fd, 0, SEEK_SET);
        memset(tbuf, 0xFF, sizeof(tbuf));
        ok("what it kept is what was there",
           read(fd, tbuf, 8) == 8 && memcmp(tbuf, "abc", 3) == 0);
        ok("and what it grew into reads as zeros",
           tbuf[3] == 0 && tbuf[4] == 0 && tbuf[5] == 0 &&
           tbuf[6] == 0 && tbuf[7] == 0);
        close(fd);
        ok("and it can be removed", unlink("trunc") == 0);
    }

    /* --- what the kernel refuses --------------------------------- */

    ok("rmdir a non-empty directory fails",
       rmdir("drive_c") < 0 && errno == ENOTEMPTY);
    ok("chdir to a file is ENOTDIR",
       chdir("drive_c/windows/system32/ntdll.dll") < 0 && errno == ENOTDIR);
    ok("chdir to nothing is ENOENT",
       chdir("no_such_directory") < 0 && errno == ENOENT);
    ok("and a failed chdir did not move us", strcmp(here(), ROOT) == 0);

    /* getcwd into a buffer too small is ERANGE, not a truncated answer.
     * glibc's own getcwd(NULL, 0) depends on this being right. */
    ok("getcwd with no room is ERANGE",
       getcwd(buf, 4) == NULL && errno == ERANGE);

    /* --- leave nothing behind ------------------------------------ */

    ok("chdir out before cleaning up", chdir("/tmp") == 0);
    cleanup();
    ok("the prefix is gone", stat(ROOT, &st) < 0);

    printf("cwd64: %d failures\n", failures);

    /* A distinctive status, so the kernel can tell a real exit from
     * anything else that might end the run. */
    return failures ? 1 : 89;
}
