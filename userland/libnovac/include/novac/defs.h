/* libNovaC common definitions shared by the public headers.
 * Contains no kernel or architecture specifics. */
#ifndef NOVAC_DEFS_H
#define NOVAC_DEFS_H

#ifdef __cplusplus
# define NOVAC_BEGIN_DECLS extern "C" {
# define NOVAC_END_DECLS }
#else
# define NOVAC_BEGIN_DECLS
# define NOVAC_END_DECLS
#endif

/* Compile-time format checking where the compiler supports it. */
#if defined(__GNUC__) || defined(__clang__)
# define NOVAC_PRINTF_LIKE(fmt, first) __attribute__((format(printf, fmt, first)))
#else
# define NOVAC_PRINTF_LIKE(fmt, first)
#endif

#define NOVAC_VERSION_MAJOR 0
#define NOVAC_VERSION_MINOR 1

#endif
