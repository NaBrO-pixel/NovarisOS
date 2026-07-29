#ifndef SERIAL_H
#define SERIAL_H

#include <stdint.h>

/* COM1 debug output. The framebuffer console is the real UI, but pixels
 * are a miserable thing to assert against in an automated test - every
 * previous milestone had to OCR screenshots against the kernel's own font
 * bitmap to check what the machine actually printed. Mirroring the same
 * character stream out the serial port gives the QEMU harness
 * (tools/qemu_test.py) a plain-text transcript instead, which is what
 * makes the Win32 emulation testable at any useful scale. */
void serial_init(void);

/* True once serial_init() has run and the UART answered its loopback
 * self-test. Everything below is a no-op before that. */
int serial_ready(void);

void serial_putchar(char c);
void serial_writestring(const char* s);

#endif
