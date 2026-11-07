#ifndef NOVAC_STDLIB_H
#define NOVAC_STDLIB_H
#include <stddef.h>
#include <novac/defs.h>

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX     32767

typedef struct { int       quot, rem; } div_t;
typedef struct { long      quot, rem; } ldiv_t;
typedef struct { long long quot, rem; } lldiv_t;

NOVAC_BEGIN_DECLS
void *malloc(size_t size);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);
void  free(void *ptr);

_Noreturn void abort(void);
_Noreturn void exit(int status);
_Noreturn void _Exit(int status);
int   atexit(void (*fn)(void));
char *getenv(const char *name);
int   setenv(const char *name, const char *value, int overwrite);
int   unsetenv(const char *name);

int       atoi(const char *s);
long      atol(const char *s);
long long atoll(const char *s);
long               strtol(const char *restrict s, char **restrict end, int base);
long long          strtoll(const char *restrict s, char **restrict end, int base);
unsigned long      strtoul(const char *restrict s, char **restrict end, int base);
unsigned long long strtoull(const char *restrict s, char **restrict end, int base);

int       abs(int x);
long      labs(long x);
long long llabs(long long x);
div_t     div(int n, int d);
ldiv_t    ldiv(long n, long d);
lldiv_t   lldiv(long long n, long long d);

void  qsort(void *base, size_t nmemb, size_t size,
            int (*cmp)(const void *, const void *));
void *bsearch(const void *key, const void *base, size_t nmemb, size_t size,
              int (*cmp)(const void *, const void *));

/* Linear congruential generator. NOT suitable for security purposes. */
int   rand(void);
void  srand(unsigned int seed);
NOVAC_END_DECLS
#endif
