/* Buffered FILE streams over file descriptors.
 *
 * One buffer per stream serves either reading or writing, never both at once;
 * switching direction flushes or rewinds first. stdout is line buffered,
 * stderr unbuffered, stdin fully buffered (there is no isatty, so no
 * terminal detection). Not thread-safe. fflush on an input stream is a no-op.
 * Unsupported: fscanf/scanf, tmpfile, freopen, fgetpos/fsetpos, wide I/O. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <limits.h>
#include <novac/format.h>
#include "../internal/exit_hook.h"

enum { F_READ = 1, F_WRITE = 2, F_EOF = 4, F_ERR = 8, F_OWNBUF = 16, F_STATIC = 32 };

struct novac_file {
    int fd, flags, bufmode, ungot;
    unsigned char *buf;
    size_t bufsize, rpos, rend, wlen;
    unsigned char tiny[1];
    struct novac_file *next;
};

static unsigned char in_buf[BUFSIZ], out_buf[BUFSIZ];
static struct novac_file f_err = { 2, F_WRITE | F_STATIC, _IONBF, -1, NULL, 0, 0, 0, 0, {0}, NULL };
static struct novac_file f_out = { 1, F_WRITE | F_STATIC, _IOLBF, -1, out_buf, BUFSIZ, 0, 0, 0, {0}, &f_err };
static struct novac_file f_in  = { 0, F_READ  | F_STATIC, _IOFBF, -1, in_buf, BUFSIZ, 0, 0, 0, {0}, &f_out };

FILE *novac_stdin = &f_in, *novac_stdout = &f_out, *novac_stderr = &f_err;
static FILE *open_list = &f_in;
static int exit_hooked;

static void flush_all_at_exit(void) { fflush(NULL); }
static void hook_exit(void)
{
    if (!exit_hooked) { exit_hooked = 1; novac__set_exit_flush(flush_all_at_exit); }
}

static int flush_write(FILE *f)
{
    size_t off = 0;
    while (off < f->wlen) {
        ssize_t w = write(f->fd, f->buf + off, f->wlen - off);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) {
            if (w == 0) errno = EIO;
            f->flags |= F_ERR;
            f->wlen = 0;                       /* data is lost; the error flag says so */
            return -1;
        }
        off += (size_t)w;
    }
    f->wlen = 0;
    return 0;
}

/* Give back buffered-but-unconsumed input so fd's offset equals the stream's. */
static void drop_read(FILE *f)
{
    size_t unread = (f->rend - f->rpos) + (f->ungot >= 0 ? 1u : 0u);
    if (unread) {
        int saved = errno;
        lseek(f->fd, -(off_t)unread, SEEK_CUR);   /* fails harmlessly on pipes */
        errno = saved;
    }
    f->rpos = f->rend = 0;
    f->ungot = -1;
}

static int ensure_buf(FILE *f)
{
    if (f->buf) return 0;
    if (f->bufmode != _IONBF) {
        f->buf = malloc(BUFSIZ);
        if (f->buf) { f->bufsize = BUFSIZ; f->flags |= F_OWNBUF; return 0; }
    }
    f->buf = f->tiny; f->bufsize = 1; f->bufmode = _IONBF;   /* degrade, don't fail */
    return 0;
}

static int fill(FILE *f)
{
    ssize_t n;
    if (f->wlen && flush_write(f) != 0) return -1;
    ensure_buf(f);
    do n = read(f->fd, f->buf, f->bufsize); while (n < 0 && errno == EINTR);
    if (n < 0) { f->flags |= F_ERR; return -1; }
    if (n == 0) { f->flags |= F_EOF; return -1; }
    f->rpos = 0; f->rend = (size_t)n;
    return 0;
}

static int put_bytes(FILE *f, const void *data, size_t n)
{
    const unsigned char *p = data;
    size_t left = n;
    hook_exit();
    if (!(f->flags & F_WRITE)) { errno = EBADF; f->flags |= F_ERR; return -1; }
    if (f->rend > f->rpos || f->ungot >= 0) drop_read(f);
    ensure_buf(f);
    while (left) {
        size_t room = f->bufsize - f->wlen;
        if (room == 0) { if (flush_write(f) != 0) return -1; room = f->bufsize; }
        if (f->wlen == 0 && left >= f->bufsize && f->bufsize > 1) {   /* big write: skip the copy */
            size_t off = 0;
            while (off < left) {
                ssize_t w = write(f->fd, p + off, left - off);
                if (w < 0 && errno == EINTR) continue;
                if (w <= 0) { if (w == 0) errno = EIO; f->flags |= F_ERR; return -1; }
                off += (size_t)w;
            }
            return 0;
        }
        { size_t k = left < room ? left : room;
          memcpy(f->buf + f->wlen, p, k);
          f->wlen += k; p += k; left -= k; }
    }
    if (f->bufmode == _IONBF || (f->bufmode == _IOLBF && memchr(data, '\n', n)))
        return flush_write(f);
    return 0;
}

FILE *fopen(const char *path, const char *mode)
{
    int flags, acc, plus = 0;
    FILE *f;
    int fd;
    if (!path || !mode) { errno = EINVAL; return NULL; }
    switch (mode[0]) {
    case 'r': acc = 0; flags = 0; break;
    case 'w': acc = 1; flags = O_CREAT | O_TRUNC; break;
    case 'a': acc = 1; flags = O_CREAT | O_APPEND; break;
    default:  errno = EINVAL; return NULL;
    }
    for (const char *m = mode + 1; *m; m++) {
        if (*m == '+') plus = 1;
        else if (*m != 'b') { errno = EINVAL; return NULL; }
    }
    if (plus) acc = O_RDWR;
    f = calloc(1, sizeof *f);
    if (!f) return NULL;                         /* errno = ENOMEM from calloc */
    fd = open(path, flags | acc, 0666);
    if (fd < 0) { free(f); return NULL; }
    f->fd = fd;
    f->flags = (acc == O_RDWR ? F_READ | F_WRITE : acc == 1 ? F_WRITE : F_READ);
    f->bufmode = _IOFBF;
    f->ungot = -1;
    f->next = open_list;
    open_list = f;
    return f;
}

int fflush(FILE *f)
{
    int rc = 0;
    if (!f) {
        for (FILE *p = open_list; p; p = p->next)
            if (p->wlen && flush_write(p) != 0) rc = EOF;
        return rc;
    }
    return f->wlen && flush_write(f) != 0 ? EOF : 0;
}

int fclose(FILE *f)
{
    int rc = 0;
    FILE **pp;
    if (!f) { errno = EINVAL; return EOF; }
    if (f->wlen && flush_write(f) != 0) rc = EOF;
    if (f->rend > f->rpos || f->ungot >= 0) drop_read(f);
    if (f->fd >= 0 && close(f->fd) != 0) rc = EOF;
    for (pp = &open_list; *pp && *pp != f; pp = &(*pp)->next) {}
    if (*pp) *pp = f->next;
    if (f->flags & F_OWNBUF) free(f->buf);
    if (f->flags & F_STATIC) { f->fd = -1; f->flags = F_STATIC; f->buf = NULL; f->bufsize = 0; f->next = NULL; }
    else free(f);
    return rc;
}

int fgetc(FILE *f)
{
    if (f->ungot >= 0) { int c = f->ungot; f->ungot = -1; return c; }
    if (!(f->flags & F_READ)) { errno = EBADF; f->flags |= F_ERR; return EOF; }
    if (f->flags & F_EOF) return EOF;
    if (f->rpos == f->rend && fill(f) != 0) return EOF;
    return f->buf[f->rpos++];
}
int getc(FILE *f)  { return fgetc(f); }
int getchar(void)  { return fgetc(stdin); }

int ungetc(int c, FILE *f)
{
    if (c == EOF || f->ungot >= 0 || !(f->flags & F_READ)) return EOF;
    f->ungot = (unsigned char)c;
    f->flags &= ~F_EOF;
    return (unsigned char)c;
}

size_t fread(void *buf, size_t size, size_t n, FILE *f)
{
    unsigned char *p = buf;
    size_t total, got = 0;
    if (size == 0 || n == 0) return 0;
    if (n > (size_t)-1 / size) { errno = EOVERFLOW; f->flags |= F_ERR; return 0; }
    total = size * n;
    while (got < total) {
        int c = fgetc(f);
        if (c == EOF) break;
        p[got++] = (unsigned char)c;
    }
    return got / size;
}

char *fgets(char *buf, int n, FILE *f)
{
    int i = 0;
    if (n <= 0) return NULL;
    while (i < n - 1) {
        int c = fgetc(f);
        if (c == EOF) break;
        buf[i++] = (char)c;
        if (c == '\n') break;
    }
    if (i == 0) return NULL;
    buf[i] = '\0';
    return buf;
}

size_t fwrite(const void *buf, size_t size, size_t n, FILE *f)
{
    size_t total;
    if (size == 0 || n == 0) return 0;
    if (n > (size_t)-1 / size) { errno = EOVERFLOW; f->flags |= F_ERR; return 0; }
    total = size * n;
    return put_bytes(f, buf, total) == 0 ? n : 0;   /* partial counts are not tracked */
}

int fputc(int c, FILE *f)
{
    unsigned char ch = (unsigned char)c;
    return put_bytes(f, &ch, 1) == 0 ? ch : EOF;
}
int putc(int c, FILE *f) { return fputc(c, f); }
int putchar(int c)       { return fputc(c, stdout); }
int fputs(const char *s, FILE *f) { return put_bytes(f, s, strlen(s)) == 0 ? 1 : EOF; }
int puts(const char *s)
{
    return (put_bytes(stdout, s, strlen(s)) == 0 && put_bytes(stdout, "\n", 1) == 0) ? 1 : EOF;
}

int fseek(FILE *f, long off, int whence)
{
    if (f->wlen && flush_write(f) != 0) return -1;
    if (f->rend > f->rpos || f->ungot >= 0) drop_read(f);
    if (lseek(f->fd, off, whence) < 0) return -1;
    f->flags &= ~F_EOF;
    return 0;
}

long ftell(FILE *f)
{
    off_t pos = lseek(f->fd, 0, SEEK_CUR);
    if (pos < 0) return -1;
    return (long)pos + (long)f->wlen - (long)(f->rend - f->rpos) - (f->ungot >= 0 ? 1 : 0);
}

void rewind(FILE *f) { fseek(f, 0, SEEK_SET); f->flags &= ~F_ERR; }
int  feof(FILE *f)   { return (f->flags & F_EOF) != 0; }
int  ferror(FILE *f) { return (f->flags & F_ERR) != 0; }
void clearerr(FILE *f) { f->flags &= ~(F_EOF | F_ERR); }
int  fileno(FILE *f) { if (f->fd < 0) { errno = EBADF; return -1; } return f->fd; }

int setvbuf(FILE *f, char *buf, int mode, size_t size)
{
    if ((mode != _IOFBF && mode != _IOLBF && mode != _IONBF) ||
        f->wlen || f->rend > f->rpos) { errno = EINVAL; return -1; }
    if (f->flags & F_OWNBUF) { free(f->buf); f->flags &= ~F_OWNBUF; f->buf = NULL; f->bufsize = 0; }
    f->bufmode = mode;
    if (mode != _IONBF && buf && size > 0) { f->buf = (unsigned char *)buf; f->bufsize = size; }
    else if (mode == _IONBF) { f->buf = NULL; f->bufsize = 0; }
    else if (f->buf && !(f->flags & F_OWNBUF)) { f->buf = NULL; f->bufsize = 0; }
    return 0;
}

int remove(const char *path)
{
    if (unlink(path) == 0) return 0;
    if (errno == EISDIR || errno == EPERM) return rmdir(path);
    return -1;
}

typedef struct { FILE *f; int err; } stream_ctx_t;
static void stream_sink(void *c, const char *s, size_t n)
{
    stream_ctx_t *ctx = c;
    if (!ctx->err && put_bytes(ctx->f, s, n) != 0) ctx->err = 1;
}

int vfprintf(FILE *restrict f, const char *restrict fmt, va_list ap)
{
    stream_ctx_t ctx = { f, 0 };
    int r = novac_format(stream_sink, &ctx, fmt, ap);
    return ctx.err ? -1 : r;
}
int vprintf(const char *restrict fmt, va_list ap) { return vfprintf(stdout, fmt, ap); }
int fprintf(FILE *restrict f, const char *restrict fmt, ...)
{ va_list ap; int r; va_start(ap, fmt); r = vfprintf(f, fmt, ap); va_end(ap); return r; }
int printf(const char *restrict fmt, ...)
{ va_list ap; int r; va_start(ap, fmt); r = vfprintf(stdout, fmt, ap); va_end(ap); return r; }

void perror(const char *s)
{
    int e = errno;
    if (s && *s) fprintf(stderr, "%s: ", s);
    fprintf(stderr, "%s\n", strerror(e));
}
