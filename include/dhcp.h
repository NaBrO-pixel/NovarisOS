#ifndef DHCP_H
#define DHCP_H

#include <stdint.h>

/* Asks for an address and configures the stack with what comes back.
 * Returns 1 if the interface now has an address. Blocks - by polling, not
 * by sleeping - for up to `timeout_ticks` PIT ticks (100 per second). */
int dhcp_configure(uint32_t timeout_ticks);

#endif /* DHCP_H */
