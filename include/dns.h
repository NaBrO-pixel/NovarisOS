#ifndef DNS_H
#define DNS_H

#include <stdint.h>

/* Resolves a name to an address in host order. A dotted quad resolves to
 * itself without a query. Returns 1 on success. Polls rather than
 * blocking - see the note at the top of kernel/net.c. */
int dns_resolve(const char* name, uint32_t* out, uint32_t timeout_ticks);

#endif /* DNS_H */
