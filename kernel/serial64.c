/* serial64.c - polling COM1 output for the 64-bit bring-up. See serial64.h
 * for why this is not kernel/serial.c. */

#include "serial64.h"
#include "io.h"

#define COM1 0x3F8

#define UART_DATA        0
#define UART_INT_ENABLE  1
#define UART_FIFO_CTRL   2
#define UART_LINE_CTRL   3
#define UART_MODEM_CTRL  4
#define UART_LINE_STATUS 5

#define LSR_THR_EMPTY 0x20

void serial64_init(void) {
    outb(COM1 + UART_INT_ENABLE, 0x00); /* no interrupts: we poll */
    outb(COM1 + UART_LINE_CTRL,  0x80); /* DLAB on */
    outb(COM1 + UART_DATA,       0x03); /* divisor 3 => 38400 baud */
    outb(COM1 + UART_INT_ENABLE, 0x00);
    outb(COM1 + UART_LINE_CTRL,  0x03); /* DLAB off, 8N1 */
    outb(COM1 + UART_FIFO_CTRL,  0xC7); /* enable and clear the FIFOs */
    outb(COM1 + UART_MODEM_CTRL, 0x0F); /* DTR/RTS/OUT2 */
}

void serial64_putc(char c) {
    if (c == '\n') {
        while (!(inb(COM1 + UART_LINE_STATUS) & LSR_THR_EMPTY)) { }
        outb(COM1 + UART_DATA, '\r');
    }
    while (!(inb(COM1 + UART_LINE_STATUS) & LSR_THR_EMPTY)) { }
    outb(COM1 + UART_DATA, (uint8_t)c);
}

void serial64_puts(const char* s) {
    while (*s) serial64_putc(*s++);
}

void serial64_puthex(uint64_t v) {
    static const char digits[] = "0123456789abcdef";
    char buf[17];
    int i;

    buf[16] = '\0';
    for (i = 15; i >= 0; i--) {
        buf[i] = digits[v & 0xF];
        v >>= 4;
    }
    serial64_puts("0x");
    serial64_puts(buf);
}

void serial64_putdec(uint64_t v) {
    char buf[21];
    int i = 20;

    buf[20] = '\0';
    if (v == 0) { serial64_putc('0'); return; }
    while (v && i > 0) {
        buf[--i] = (char)('0' + (v % 10));
        v /= 10;
    }
    serial64_puts(&buf[i]);
}
