#include "mouse.h"
#include "io.h"
#include "idt.h"
#include "pic.h"
#include "console.h"
#include "framebuffer.h"

#define I8042_DATA   0x60
#define I8042_STATUS 0x64
#define I8042_CMD    0x64

#define I8042_STATUS_OUTPUT_FULL 0x01
#define I8042_STATUS_INPUT_FULL  0x02

/* Simple pointer-shaped cursor, 11 pixels wide by 16 tall: a filled
 * triangle with a black outline so it stays visible against light or
 * dark backgrounds. Computed procedurally rather than as a fixed bitmap. */
#define CURSOR_W 11
#define CURSOR_H 16

static int cursor_x = 0, cursor_y = 0;
static uint32_t cursor_saved[CURSOR_H][CURSOR_W];
static int cursor_visible = 0;

static int cursor_shape_pixel(int row, int col) {
    int width;
    if (row <= 10) {
        width = row + 1;
    } else if (row <= 12) {
        width = 11 - (row - 10) * 3;
    } else {
        width = 2;
    }
    if (width < 1) width = 1;
    if (col >= width) return 0;
    if (col == width - 1 || row == 0) return 2; /* outline */
    return 1;                                    /* fill */
}

static void cursor_restore(void) {
    if (!cursor_visible) return;
    for (int row = 0; row < CURSOR_H; row++) {
        for (int col = 0; col < CURSOR_W; col++) {
            if (cursor_shape_pixel(row, col)) {
                fb_put_pixel((uint32_t)(cursor_x + col), (uint32_t)(cursor_y + row),
                             cursor_saved[row][col]);
            }
        }
    }
}

static void cursor_draw(void) {
    for (int row = 0; row < CURSOR_H; row++) {
        for (int col = 0; col < CURSOR_W; col++) {
            int shape = cursor_shape_pixel(row, col);
            if (!shape) continue;
            uint32_t px = (uint32_t)(cursor_x + col);
            uint32_t py = (uint32_t)(cursor_y + row);
            cursor_saved[row][col] = fb_get_pixel(px, py);
            fb_put_pixel(px, py, shape == 2 ? fb_rgb(0, 0, 0) : fb_rgb(255, 255, 255));
        }
    }
    cursor_visible = 1;
}

static void cursor_move_to(int x, int y) {
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if ((uint32_t)x >= fb_width()) x = (int)fb_width() - 1;
    if ((uint32_t)y >= fb_height()) y = (int)fb_height() - 1;

    cursor_restore();
    cursor_x = x;
    cursor_y = y;
    cursor_draw();
}

static void mouse_wait_write(void) {
    int timeout = 100000;
    while (timeout-- && (inb(I8042_STATUS) & I8042_STATUS_INPUT_FULL)) { }
}

static void mouse_wait_read(void) {
    int timeout = 100000;
    while (timeout-- && !(inb(I8042_STATUS) & I8042_STATUS_OUTPUT_FULL)) { }
}

static void mouse_write(uint8_t val) {
    mouse_wait_write();
    outb(I8042_CMD, 0xD4); /* "next byte on the data port goes to the mouse" */
    mouse_wait_write();
    outb(I8042_DATA, val);
}

static uint8_t mouse_read(void) {
    mouse_wait_read();
    return inb(I8042_DATA);
}

static uint8_t packet[3];
static int packet_index = 0;

static void mouse_handler(registers_t* regs) {
    (void)regs;
    uint8_t data = inb(I8042_DATA);

    /* Byte 0 of a standard PS/2 packet always has bit 3 set - use that to
     * resync if we ever start mid-packet. */
    if (packet_index == 0 && !(data & 0x08)) return;

    packet[packet_index++] = data;
    if (packet_index < 3) return;
    packet_index = 0;

    uint8_t flags = packet[0];
    int8_t dx = (int8_t)packet[1];
    int8_t dy = (int8_t)packet[2];
    if (flags & 0x40 || flags & 0x80) return; /* overflow - drop this packet */

    if (console_has_framebuffer()) {
        /* Screen Y grows downward; mouse dy is reported inverted. */
        cursor_move_to(cursor_x + dx, cursor_y - dy);
    }
}

void mouse_install(void) {
    outb(I8042_CMD, 0xA8); /* enable the auxiliary (mouse) port */

    mouse_wait_write();
    outb(I8042_CMD, 0x20); /* "read controller command byte" */
    uint8_t status = mouse_read();
    status |= 0x02;   /* enable IRQ12 */
    status &= ~0x20u; /* enable the mouse's clock line */
    mouse_wait_write();
    outb(I8042_CMD, 0x60); /* "write controller command byte" */
    mouse_wait_write();
    outb(I8042_DATA, status);

    mouse_write(0xF6); /* set defaults */
    mouse_read();       /* ACK */
    mouse_write(0xF4); /* enable data reporting */
    mouse_read();       /* ACK */

    register_interrupt_handler(44, mouse_handler); /* IRQ12 -> remapped vector 44 */
    pic_unmask_irq(2);  /* cascade line - slave PIC can't interrupt without it */
    pic_unmask_irq(12);

    if (console_has_framebuffer()) {
        cursor_x = (int)fb_width() / 2;
        cursor_y = (int)fb_height() / 2;
        cursor_draw();
    }
}
