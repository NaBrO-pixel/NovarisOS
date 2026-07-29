#ifndef IO_H
#define IO_H

#include <stdint.h>

/* Thin wrappers around the x86 IN/OUT instructions. Every hardware driver
 * (PIC, PIT, keyboard, ...) talks to its device through port I/O, so this
 * header is shared by all of them. */

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ __volatile__("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ __volatile__("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void io_wait(void) {
    /* Port 0x80 is used for POST diagnostic codes on real hardware, so
     * writing to it is a well-known "burn a few cycles" trick to give
     * slow legacy hardware (like the 8259 PIC) time to react. */
    outb(0x80, 0);
}

#endif
