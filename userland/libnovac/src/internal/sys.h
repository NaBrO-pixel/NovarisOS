/* System-call abstraction layer: the ONLY kernel-facing surface the portable
 * library depends on. A NovarisOS port implements these three functions on
 * top of the real kernel interfaces. Nothing else in libNovaC talks to the
 * kernel. */
#ifndef NOVAC_INTERNAL_SYS_H
#define NOVAC_INTERNAL_SYS_H
#include <stddef.h>

/* Map zero-initialisable, read/write, private anonymous memory.
 * In:  *size = bytes wanted.
 * Out: *size = bytes actually granted (>= requested, a multiple of 16).
 * Returns a 16-byte-aligned address or NULL on failure. */
void *novac_sys_map(size_t *size);

/* Release a region previously returned by novac_sys_map, passing the exact
 * granted size. Returns 0 on success, nonzero on failure. */
int novac_sys_unmap(void *addr, size_t size);

/* Terminate the calling process with `status`. Never returns. */
_Noreturn void novac_sys_exit(int status);

#endif
