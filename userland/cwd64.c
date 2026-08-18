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
