#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <assert.h>
#include "harness.h"

static int order[4], order_n;
static void h1(void) { order[order_n++] = 1; }
static void h2(void) { order[order_n++] = 2; }
static void do_exit(void)  { atexit(h1); atexit(h2); exit(7); }
static void do_Exit(void)  { _Exit(3); }
static void do_abort(void) { abort(); }

static int asserted;
static void on_assert(const char *e, const char *f, int l, const char *fn)
{ (void)e; (void)f; (void)l; (void)fn; asserted = 1; }
static void do_assert_fail(void) { assert(1 == 2); }
static void do_assert_ok(void)   { assert(1 == 1); }

static int cmp_int(const void *a, const void *b)
{ int x = *(const int *)a, y = *(const int *)b; return (x > y) - (x < y); }
static int cmp_u16(const void *a, const void *b)
{ return (int)*(const unsigned short *)a - (int)*(const unsigned short *)b; }

void test_misc(void)
{
    int st = -1, r;

    /* errno is writable, readable, and a stable lvalue */
    errno = 0; CHECK_EQ(errno, 0);
    errno = ERANGE; CHECK_EQ(errno, ERANGE);
    errno = 0;

    /* abs family */
    CHECK_EQ(abs(-5), 5); CHECK_EQ(abs(5), 5); CHECK_EQ(abs(0), 0);
    CHECK_EQ(abs(INT_MIN), INT_MIN);            /* documented: no trap, unchanged */
    CHECK_EQ(labs(-5L), 5L);
    CHECK(llabs(-5000000000LL) == 5000000000LL);
    { div_t d = div(-7, 2); CHECK_EQ(d.quot, -3); CHECK_EQ(d.rem, -1); }
    { ldiv_t d = ldiv(7L, -2L); CHECK_EQ(d.quot, -3); CHECK_EQ(d.rem, 1); }
    { lldiv_t d = lldiv(9LL, 4LL); CHECK_EQ(d.quot, 2); CHECK_EQ(d.rem, 1); }

    /* qsort: empty, single, sorted, reversed, duplicates, other element sizes */
    { int a[1] = {5}; qsort(a, 1, sizeof a[0], cmp_int); CHECK_EQ(a[0], 5);
      qsort(a, 0, sizeof a[0], cmp_int); }
    { int a[] = {5, 2, 9, 1, 5, 5, 0, -3, 8}; int want[] = {-3, 0, 1, 2, 5, 5, 5, 8, 9}; size_t i;
      qsort(a, 9, sizeof a[0], cmp_int);
      for (i = 0; i < 9; i++) CHECK_EQ(a[i], want[i]); }
    { int a[200]; int i, ok = 1;
      for (i = 0; i < 200; i++) a[i] = 200 - i;
      qsort(a, 200, sizeof a[0], cmp_int);
      for (i = 0; i < 200; i++) if (a[i] != i + 1) ok = 0;
      CHECK(ok);
      qsort(a, 200, sizeof a[0], cmp_int);       /* already sorted */
      for (i = 0; i < 200; i++) if (a[i] != i + 1) ok = 0;
      CHECK(ok); }
    { unsigned short a[] = {300, 2, 65535, 0, 7}; unsigned short w[] = {0, 2, 7, 300, 65535}; size_t i;
      qsort(a, 5, sizeof a[0], cmp_u16);
      for (i = 0; i < 5; i++) CHECK_EQ(a[i], w[i]); }
    { unsigned char a[] = {3, 1, 2};
      errno = 0; qsort(a, 3, 0, NULL);            /* size 0: nothing to do */
      CHECK_EQ(errno, 0);
      errno = 0; qsort(a, 3, 1, NULL);            /* NULL comparator reported */
      CHECK_EQ(errno, EINVAL); errno = 0; }

    /* bsearch */
    { int a[] = {1, 3, 5, 7, 9, 11}; int k;
      for (k = 0; k < 6; k++) { int key = a[k]; CHECK(bsearch(&key, a, 6, sizeof a[0], cmp_int) == &a[k]); }
      { int key = 4; CHECK(bsearch(&key, a, 6, sizeof a[0], cmp_int) == NULL); }
      { int key = 1; CHECK(bsearch(&key, a, 0, sizeof a[0], cmp_int) == NULL); } }

    /* rand: deterministic per seed, in range, seeds differ */
    { int a, b, i, ok = 1;
      srand(42); a = rand(); srand(42); b = rand();
      CHECK_EQ(a, b);
      for (i = 0; i < 1000; i++) { r = rand(); if (r < 0 || r > RAND_MAX) ok = 0; }
      CHECK(ok);
      srand(1); a = rand(); srand(2); b = rand();
      CHECK(a != b); }

    /* exit runs atexit handlers in reverse order, then terminates with status */
    CHECK(th_run_expect_exit(do_exit, &st));
    CHECK_EQ(st, 7); CHECK_EQ(order_n, 2); CHECK_EQ(order[0], 2); CHECK_EQ(order[1], 1);
    CHECK(th_run_expect_exit(do_Exit, &st)); CHECK_EQ(st, 3);   /* _Exit skips handlers */
    CHECK_EQ(order_n, 2);
    CHECK(th_run_expect_exit(do_abort, &st)); CHECK_EQ(st, 134);
    errno = 0; CHECK_EQ(atexit(NULL), -1); CHECK_EQ(errno, EINVAL);
    { int n = 0;                                  /* table limit is reported, not ignored */
      while (atexit(h1) == 0 && n < 1000) n++;
      CHECK_EQ(n, 32); CHECK_EQ(errno, ENOMEM); }

    /* assert: handler runs, then process aborts; passing assert is silent */
    novac_set_assert_handler(on_assert);
    CHECK(!th_run_expect_exit(do_assert_ok, &st)); CHECK_EQ(asserted, 0);
    CHECK(th_run_expect_exit(do_assert_fail, &st));
    CHECK_EQ(st, 134); CHECK_EQ(asserted, 1);
    novac_set_assert_handler(NULL);
    CHECK(th_run_expect_exit(do_assert_fail, &st)); CHECK_EQ(st, 134);
}
