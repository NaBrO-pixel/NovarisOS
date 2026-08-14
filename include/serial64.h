#ifndef SERIAL64_H
#define SERIAL64_H

#include <stdint.h>

/* A polling COM1 driver for the 64-bit bring-up.
 *
 * Separate from kernel/serial.c on purpose and only for as long as the
 * port lasts: the real one is wired into console.c, which pulls in the
 * framebuffer, the VGA text driver and the window manager. During bring-up
 * the point of the serial port is to report on subsystems that do not work
 * yet, so it cannot depend on any of them. */

void serial64_init(void);
void serial64_putc(char c);
void serial64_puts(const char* s);
void serial64_puthex(uint64_t v);
void serial64_putdec(uint64_t v);

#endif
