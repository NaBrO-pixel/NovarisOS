/* Formatting into memory (portable) and buffered FILE streams over file
 * descriptors (system layer; needs the NovarisOS/Linux syscall binding). */
#ifndef NOVAC_STDIO_H
#define NOVAC_STDIO_H
#include <stddef.h>
#include <stdarg.h>
#include <novac/defs.h>

NOVAC_BEGIN_DECLS
int snprintf(char *restrict buf, size_t n, const char *restrict fmt, ...)
    NOVAC_PRINTF_LIKE(3, 4);
int vsnprintf(char *restrict buf, size_t n, const char *restrict fmt, va_list ap)
    NOVAC_PRINTF_LIKE(3, 0);
/* Unbounded by definition; prefer snprintf. Writes at most INT_MAX bytes. */
int sprintf(char *restrict buf, const char *restrict fmt, ...)
    NOVAC_PRINTF_LIKE(2, 3);
int vsprintf(char *restrict buf, const char *restrict fmt, va_list ap)
    NOVAC_PRINTF_LIKE(2, 0);

#define EOF      (-1)
#define BUFSIZ   1024
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define _IOFBF 0
#define _IOLBF 1
#define _IONBF 2

typedef struct novac_file FILE;
extern FILE *novac_stdin, *novac_stdout, *novac_stderr;
#define stdin  novac_stdin
#define stdout novac_stdout
#define stderr novac_stderr

FILE  *fopen(const char *path, const char *mode);
int    fclose(FILE *f);
int    fflush(FILE *f);            /* fflush(NULL) flushes every open stream */
size_t fread(void *buf, size_t size, size_t n, FILE *f);
size_t fwrite(const void *buf, size_t size, size_t n, FILE *f);
int    fgetc(FILE *f);
int    getc(FILE *f);
int    getchar(void);
int    ungetc(int c, FILE *f);
char  *fgets(char *buf, int n, FILE *f);
int    fputc(int c, FILE *f);
int    putc(int c, FILE *f);
int    putchar(int c);
int    fputs(const char *s, FILE *f);
int    puts(const char *s);
int    fseek(FILE *f, long off, int whence);
long   ftell(FILE *f);
void   rewind(FILE *f);
int    feof(FILE *f);
int    ferror(FILE *f);
void   clearerr(FILE *f);
int    fileno(FILE *f);
int    setvbuf(FILE *f, char *buf, int mode, size_t size);
int    remove(const char *path);
int    rename(const char *from, const char *to);
void   perror(const char *s);
int    printf(const char *restrict fmt, ...) NOVAC_PRINTF_LIKE(1, 2);
int    vprintf(const char *restrict fmt, va_list ap) NOVAC_PRINTF_LIKE(1, 0);
int    fprintf(FILE *restrict f, const char *restrict fmt, ...) NOVAC_PRINTF_LIKE(2, 3);
int    vfprintf(FILE *restrict f, const char *restrict fmt, va_list ap) NOVAC_PRINTF_LIKE(2, 0);
NOVAC_END_DECLS
#endif
