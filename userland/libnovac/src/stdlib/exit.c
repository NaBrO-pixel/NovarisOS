#include <stdlib.h>
#include <errno.h>
#include "../internal/sys.h"
#include "../internal/exit_hook.h"

#define NOVAC_ATEXIT_MAX   32
#define NOVAC_ABORT_STATUS 134   /* conventional 128 + SIGABRT; see README */

static void (*handlers[NOVAC_ATEXIT_MAX])(void);
static size_t handler_count;
static void (*exit_flush)(void);

void novac__set_exit_flush(void (*fn)(void)) { exit_flush = fn; }

int atexit(void (*fn)(void))
{
    if (!fn)                              { errno = EINVAL; return -1; }
    if (handler_count == NOVAC_ATEXIT_MAX) { errno = ENOMEM; return -1; }
    handlers[handler_count++] = fn;
    return 0;
}

_Noreturn void exit(int status)
{
    while (handler_count > 0)             /* reverse registration order */
        handlers[--handler_count]();
    if (exit_flush) exit_flush();          /* stdio buffers, after atexit handlers */
    novac_sys_exit(status);
}

_Noreturn void _Exit(int status) { novac_sys_exit(status); }

_Noreturn void abort(void) { novac_sys_exit(NOVAC_ABORT_STATUS); }
