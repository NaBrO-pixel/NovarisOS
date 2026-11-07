#ifndef NOVAC_SYS_STAT_H
#define NOVAC_SYS_STAT_H
#include <sys/types.h>
#include <time.h>
#include <novac/defs.h>

/* Linux x86-64 struct stat layout (144 bytes); checked by a static assert in
 * the implementation. */
struct stat {
    dev_t st_dev; ino_t st_ino; nlink_t st_nlink;
    mode_t st_mode; uid_t st_uid; gid_t st_gid; unsigned int __pad0;
    dev_t st_rdev; off_t st_size; blksize_t st_blksize; blkcnt_t st_blocks;
    struct timespec st_atim, st_mtim, st_ctim;
    long __unused[3];
};

#define S_IFMT   0170000
#define S_IFSOCK 0140000
#define S_IFLNK  0120000
#define S_IFREG  0100000
#define S_IFBLK  0060000
#define S_IFDIR  0040000
#define S_IFCHR  0020000
#define S_IFIFO  0010000
#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
#define S_ISLNK(m)  (((m) & S_IFMT) == S_IFLNK)
#define S_ISCHR(m)  (((m) & S_IFMT) == S_IFCHR)
#define S_ISBLK(m)  (((m) & S_IFMT) == S_IFBLK)
#define S_ISFIFO(m) (((m) & S_IFMT) == S_IFIFO)
#define S_IRWXU 0700
#define S_IRUSR 0400
#define S_IWUSR 0200
#define S_IXUSR 0100
#define S_IRWXG 0070
#define S_IRWXO 0007

NOVAC_BEGIN_DECLS
int stat(const char *path, struct stat *st);
int lstat(const char *path, struct stat *st);
int fstat(int fd, struct stat *st);
int mkdir(const char *path, mode_t mode);
NOVAC_END_DECLS
#endif
