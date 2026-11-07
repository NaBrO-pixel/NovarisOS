#include <assert.h>
#include <stdlib.h>

static novac_assert_handler_t handler;

novac_assert_handler_t novac_set_assert_handler(novac_assert_handler_t h)
{
    novac_assert_handler_t old = handler;
    handler = h;
    return old;
}

_Noreturn void __novac_assert_fail(const char *expr, const char *file,
                                   int line, const char *func)
{
    if (handler) handler(expr, file, line, func);
    abort();
}
