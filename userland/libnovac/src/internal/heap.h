/* Allocator introspection, used by tests only. */
#ifndef NOVAC_INTERNAL_HEAP_H
#define NOVAC_INTERNAL_HEAP_H
#include <stddef.h>

/* Walks the free list verifying structural invariants. 0 = consistent. */
int novac__heap_check(void);
/* Bytes currently obtained from novac_sys_map and not yet returned. */
size_t novac__heap_mapped_bytes(void);
#endif
