/* Reusable formatting engine. Output goes to a caller-supplied sink, so the
 * same code serves buffers (snprintf), a console, or future file streams. */
#ifndef NOVAC_FORMAT_H
#define NOVAC_FORMAT_H
#include <stddef.h>
#include <stdarg.h>
#include <novac/defs.h>

NOVAC_BEGIN_DECLS

/* Called with consecutive chunks of formatted output. */
typedef void (*novac_sink_fn)(void *ctx, const char *s, size_t n);

/* Formats `fmt` into `sink`. Returns the number of characters produced, or
 * -1 with errno set (EINVAL: bad/unsupported conversion, EOVERFLOW: output
 * would exceed INT_MAX). Output produced before an error was already sent. */
int novac_format(novac_sink_fn sink, void *ctx, const char *fmt, va_list ap);

NOVAC_END_DECLS
#endif
