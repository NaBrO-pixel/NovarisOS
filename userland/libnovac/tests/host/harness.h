/* Test harness interface. Deliberately uses only plain types so test TUs
 * (compiled against libNovaC headers) never need host headers. */
#ifndef NOVAC_TEST_HARNESS_H
#define NOVAC_TEST_HARNESS_H
#include <stddef.h>

void th_check(int cond, const char *expr, const char *file, int line);
void th_check_ll(long long got, long long want, const char *expr, const char *file, int line);
void th_check_str(const char *got, const char *want, const char *expr, const char *file, int line);

#define CHECK(c)        th_check(!!(c), #c, __FILE__, __LINE__)
#define CHECK_EQ(a, b)  th_check_ll((long long)(a), (long long)(b), #a " == " #b, __FILE__, __LINE__)
#define CHECK_STR(a, b) th_check_str((a), (b), #a " == " #b, __FILE__, __LINE__)

/* Fake kernel (host_sys.c). */
void   th_sys_fail_maps(int count);      /* fail the next `count` maps; -1 = forever */
int    th_sys_map_calls(void);
int    th_sys_unmap_calls(void);
size_t th_sys_live_bytes(void);
/* Runs fn; returns 1 and stores the status if it called novac_sys_exit. */
int    th_run_expect_exit(void (*fn)(void), int *status);

void test_string(void);
void test_ctype(void);
void test_convert(void);
void test_format(void);
void test_alloc(void);
void test_misc(void);
#endif
