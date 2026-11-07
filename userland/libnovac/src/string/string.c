/* <string.h>. Plain loops on purpose: this file must be built with
 * -ffreestanding -fno-builtin (and, on GCC, -fno-tree-loop-distribute-
 * patterns) so the compiler cannot turn these loops into calls to the very
 * functions being defined. */
#include <string.h>
#include <stdint.h>
#include <errno.h>

void *memcpy(void *restrict dst, const void *restrict src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    while (n--) *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    /* Compare as integers: relational comparison of unrelated pointers is UB. */
    if ((uintptr_t)d <= (uintptr_t)s || (uintptr_t)d >= (uintptr_t)s + n) {
        while (n--) *d++ = *s++;
    } else {
        d += n; s += n;
        while (n--) *--d = *--s;
    }
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    unsigned char *d = dst;
    while (n--) *d++ = (unsigned char)c;
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *x = a, *y = b;
    for (; n; n--, x++, y++)
        if (*x != *y) return *x < *y ? -1 : 1;
    return 0;
}

void *memchr(const void *s, int c, size_t n)
{
    const unsigned char *p = s;
    for (; n; n--, p++)
        if (*p == (unsigned char)c) return (void *)p;
    return NULL;
}

size_t strlen(const char *s)
{
    const char *p = s;
    while (*p) p++;
    return (size_t)(p - s);
}

size_t strnlen(const char *s, size_t maxlen)
{
    size_t n = 0;
    while (n < maxlen && s[n]) n++;
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a < (unsigned char)*b ? -1
         : (unsigned char)*a > (unsigned char)*b ? 1 : 0;
}

int strncmp(const char *a, const char *b, size_t n)
{
    for (; n; n--, a++, b++) {
        if (*a != *b) return (unsigned char)*a < (unsigned char)*b ? -1 : 1;
        if (!*a) return 0;
    }
    return 0;
}

char *strcpy(char *restrict dst, const char *restrict src)
{
    char *d = dst;
    while ((*d++ = *src++) != '\0') {}
    return dst;
}

char *strncpy(char *restrict dst, const char *restrict src, size_t n)
{
    size_t i = 0;
    for (; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = '\0';          /* pad, as the standard requires */
    return dst;
}

char *strcat(char *restrict dst, const char *restrict src)
{
    strcpy(dst + strlen(dst), src);
    return dst;
}

char *strncat(char *restrict dst, const char *restrict src, size_t n)
{
    char *d = dst + strlen(dst);
    while (n && *src) { *d++ = *src++; n--; }
    *d = '\0';
    return dst;
}

char *strchr(const char *s, int c)
{
    for (;; s++) {
        if (*s == (char)c) return (char *)s;   /* also matches the terminator */
        if (!*s) return NULL;
    }
}

char *strrchr(const char *s, int c)
{
    const char *last = NULL;
    for (;; s++) {
        if (*s == (char)c) last = s;
        if (!*s) return (char *)last;
    }
}

char *strstr(const char *h, const char *n)
{
    size_t nl = strlen(n);
    if (nl == 0) return (char *)h;
    for (; *h; h++)
        if (*h == *n && strncmp(h, n, nl) == 0) return (char *)h;
    return NULL;
}

char *strerror(int e)
{
    const char *m;
    switch (e) {
    case 0:         m = "Success"; break;
    case EPERM:     m = "Operation not permitted"; break;
    case ENOENT:    m = "No such file or directory"; break;
    case ESRCH:     m = "No such process"; break;
    case ETXTBSY:   m = "Text file busy"; break;
    case EDEADLK:   m = "Resource deadlock avoided"; break;
    case ENOLCK:    m = "No locks available"; break;
    case EIO:       m = "I/O error"; break;
    case EBADF:     m = "Bad file descriptor"; break;
    case ENOMEM:    m = "Out of memory"; break;
    case EACCES:    m = "Permission denied"; break;
    case EFAULT:    m = "Bad address"; break;
    case EBUSY:     m = "Resource busy"; break;
    case EEXIST:    m = "File exists"; break;
    case ENOTDIR:   m = "Not a directory"; break;
    case EISDIR:    m = "Is a directory"; break;
    case EINVAL:    m = "Invalid argument"; break;
    case EMFILE:    m = "Too many open files"; break;
    case ENOSPC:    m = "No space left on device"; break;
    case EDOM:      m = "Argument outside function domain"; break;
    case ERANGE:    m = "Result out of range"; break;
    case ENOSYS:    m = "Function not implemented"; break;
    case EOVERFLOW: m = "Value too large"; break;
    case EILSEQ:    m = "Illegal byte sequence"; break;
    case ENOTSUP:   m = "Operation not supported"; break;
    case EINTR:     m = "Interrupted system call"; break;
    case ENXIO:     m = "No such device or address"; break;
    case E2BIG:     m = "Argument list too long"; break;
    case ENOEXEC:   m = "Exec format error"; break;
    case ECHILD:    m = "No child processes"; break;
    case EAGAIN:    m = "Resource temporarily unavailable"; break;
    case EXDEV:     m = "Cross-device link"; break;
    case ENODEV:    m = "No such device"; break;
    case ENFILE:    m = "Too many open files in system"; break;
    case ENOTTY:    m = "Not a tty"; break;
    case EFBIG:     m = "File too large"; break;
    case ESPIPE:    m = "Illegal seek"; break;
    case EROFS:     m = "Read-only file system"; break;
    case EMLINK:    m = "Too many links"; break;
    case EPIPE:     m = "Broken pipe"; break;
    case ENAMETOOLONG: m = "File name too long"; break;
    case ENOTEMPTY: m = "Directory not empty"; break;
    case ELOOP:     m = "Too many levels of symbolic links"; break;
    case ETIMEDOUT: m = "Connection timed out"; break;
    default:        m = "Unknown error"; break;
    }
    return (char *)m;   /* callers must not modify, as with any strerror */
}
