/* No include guard around the macro: C requires assert to follow NDEBUG at
 * each inclusion. */
#include <novac/defs.h>

#ifndef NOVAC_ASSERT_DECLS
#define NOVAC_ASSERT_DECLS
NOVAC_BEGIN_DECLS
typedef void (*novac_assert_handler_t)(const char *expr, const char *file,
                                       int line, const char *func);
/* Optional hook, called before the process aborts (e.g. to print to a
 * console). Returns the previous handler. NULL restores the default (abort
 * only). */
novac_assert_handler_t novac_set_assert_handler(novac_assert_handler_t h);
_Noreturn void __novac_assert_fail(const char *expr, const char *file,
                                   int line, const char *func);
NOVAC_END_DECLS
#ifndef __cplusplus
# define static_assert _Static_assert
#endif
#endif

#undef assert
#ifdef NDEBUG
# define assert(e) ((void)0)
#else
# define assert(e) ((e) ? (void)0 : __novac_assert_fail(#e, __FILE__, __LINE__, __func__))
#endif
