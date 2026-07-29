#include "serial.h"
#include "io.h"

/* Standard 16550 UART at the legacy COM1 base. Register offsets are the
 * usual ones: 0 = data (and, with DLAB set, divisor low), 1 = interrupt
 * enable (divisor high with DLAB), 2 = FIFO control, 3 = line control,
 * 4 = modem control, 5 = line status. */
#define COM1_PORT 0x3F8

#define UART_DATA        0
#define UART_INT_ENABLE  1
#define UART_FIFO_CTRL   2
#define UART_LINE_CTRL   3
#define UART_MODEM_CTRL  4
#define UART_LINE_STATUS 5

#define LSR_THR_EMPTY 0x20

static int ready = 0;

void serial_init(void) {
    outb(COM1_PORT + UART_INT_ENABLE, 0x00); /* no UART interrupts - we poll */
    outb(COM1_PORT + UART_LINE_CTRL, 0x80);  /* DLAB on: next two writes set the divisor */
    outb(COM1_PORT + UART_DATA, 0x03);       /* divisor 3 => 38400 baud */
    outb(COM1_PORT + UART_INT_ENABLE, 0x00);
    outb(COM1_PORT + UART_LINE_CTRL, 0x03);  /* DLAB off, 8 bits, no parity, 1 stop */
    outb(COM1_PORT + UART_FIFO_CTRL, 0xC7);  /* enable + clear FIFOs, 14-byte trigger */
    outb(COM1_PORT + UART_MODEM_CTRL, 0x1E); /* loopback on, for the self-test below */

    /* Self-test: in loopback mode a byte written to the data register
     * should come straight back. If it doesn't, there's no usable UART
     * here (real hardware without a serial port, say) and we leave
     * `ready` at 0 so every call below turns into a no-op rather than
     * spinning forever on a status bit that will never set. */
    outb(COM1_PORT + UART_DATA, 0xAE);
    if (inb(COM1_PORT + UART_DATA) != 0xAE) return;

    outb(COM1_PORT + UART_MODEM_CTRL, 0x0F); /* normal operation: DTR/RTS/OUT2 */
    ready = 1;
}

int serial_ready(void) {
    return ready;
}

void serial_putchar(char c) {
    if (!ready) return;

    /* Bounded spin rather than `while (!empty)`: a wedged UART must not
     * be able to hang the kernel just because someone asked to print. */
    for (int spins = 0; spins < 100000; spins++) {
        if (inb(COM1_PORT + UART_LINE_STATUS) & LSR_THR_EMPTY) break;
    }

    if (c == '\n') outb(COM1_PORT + UART_DATA, '\r');
    outb(COM1_PORT + UART_DATA, (uint8_t)c);
}

void serial_writestring(const char* s) {
    while (*s) serial_putchar(*s++);
}
