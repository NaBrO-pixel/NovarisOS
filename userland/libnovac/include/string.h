#ifndef NOVAC_STRING_H
#define NOVAC_STRING_H
#include <stddef.h>
#include <novac/defs.h>

NOVAC_BEGIN_DECLS
void  *memcpy(void *restrict dst, const void *restrict src, size_t n);
void  *memmove(void *dst, const void *src, size_t n);
void  *memset(void *dst, int c, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
void  *memchr(const void *s, int c, size_t n);

size_t strlen(const char *s);
size_t strnlen(const char *s, size_t maxlen);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
char  *strcpy(char *restrict dst, const char *restrict src);
char  *strncpy(char *restrict dst, const char *restrict src, size_t n);
char  *strcat(char *restrict dst, const char *restrict src);
char  *strncat(char *restrict dst, const char *restrict src, size_t n);
char  *strchr(const char *s, int c);
char  *strrchr(const char *s, int c);
char  *strstr(const char *haystack, const char *needle);
char  *strerror(int errnum);
NOVAC_END_DECLS
#endif
