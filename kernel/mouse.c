/* mouse.c - PS/2 pointer driver.
 *
 * Milestone 7's version parsed packets and drew the cursor itself,
 * saving and restoring the pixels underneath it. That works when nothing
 * else is moving; it falls apart the moment a window can slide out from
 * under the pointer, because the saved pixels are then stale and get
 * stamped back over the new content. So this version only produces
 * events - position, buttons and wheel - and the compositor draws the
 * cursor as the topmost layer of each frame.
 */

#include "mouse.h"
#include "io.h"
#include "idt.h"
#include "pic.h"

#define I8042_DATA   0x60
#define I8042_STATUS 0x64
#define I8042_CMD    0x64

#define I8042_STATUS_OUTPUT_FULL 0x01
#define I8042_STATUS_INPUT_FULL  0x02

#define MOUSE_QUEUE_SIZE 64

static volatile mouse_event_t queue[MOUSE_QUEUE_SIZE];
static volatile uint32_t q_head = 0, q_tail = 0;

static int cursor_x = 0, cursor_y = 0;
static int bound_w = 640, bound_h = 480;
static uint8_t button_state = 0;

/* 3 for a plain mouse, 4 once the scroll wheel is negotiated. */
static int packet_size = 3;
static uint8_t packet[4];
static int packet_index = 0;

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

static void queue_push(int8_t dx, int8_t dy, int8_t wheel, uint8_t buttons) {
    uint32_t next = (q_head + 1) % MOUSE_QUEUE_SIZE;
    if (next == q_tail) return; /* full: drop, the next packet supersedes it */

    queue[q_head].x = (int16_t)cursor_x;
    queue[q_head].y = (int16_t)cursor_y;
    queue[q_head].dx = dx;
    queue[q_head].dy = dy;
    queue[q_head].wheel = wheel;
    queue[q_head].buttons = buttons;
    queue[q_head].pressed = (uint8_t)(buttons & ~button_state);
    queue[q_head].released = (uint8_t)(button_state & ~buttons);
    q_head = next;
    button_state = buttons;
}

static void mouse_handler(registers_t* regs) {
    (void)regs;
    uint8_t data = inb(I8042_DATA);

    /* Byte 0 of a packet always has bit 3 set - use that to resync if we
     * ever start reading mid-packet. */
    if (packet_index == 0 && !(data & 0x08)) return;

    packet[packet_index++] = data;
    if (packet_index < packet_size) return;
    packet_index = 0;

    uint8_t flags = packet[0];
    if (flags & 0xC0) return; /* X or Y overflow - the packet is garbage */

    int dx = (int8_t)packet[1];
    int dy = (int8_t)packet[2];
    int wheel = 0;
    if (packet_size == 4) {
        /* The low nibble is a signed 4-bit Z movement; the high nibble
         * holds buttons 4/5, which we don't report. */
        int z = packet[3] & 0x0F;
        if (z == 0x0F) wheel = -1;      /* -1 in 4-bit two's complement */
        else if (z == 0x01) wheel = 1;
    }

    cursor_x += dx;
    cursor_y -= dy; /* screen Y grows downward; the mouse reports it inverted */
    if (cursor_x < 0) cursor_x = 0;
    if (cursor_y < 0) cursor_y = 0;
    if (cursor_x > bound_w - 1) cursor_x = bound_w - 1;
    if (cursor_y > bound_h - 1) cursor_y = bound_h - 1;

    uint8_t buttons = 0;
    if (flags & 0x01) buttons |= MOUSE_BUTTON_LEFT;
    if (flags & 0x02) buttons |= MOUSE_BUTTON_RIGHT;
    if (flags & 0x04) buttons |= MOUSE_BUTTON_MIDDLE;

    queue_push((int8_t)dx, (int8_t)-dy, (int8_t)wheel, buttons);
}

/* The Intellimouse handshake: three set-sample-rate commands with a magic
 * sequence of rates makes a wheel mouse switch its device ID from 0 to 3
 * and start sending 4-byte packets. A device that doesn't know the trick
 * simply keeps reporting ID 0, and we stay on 3-byte packets. */
static void negotiate_wheel(void) {
    static const uint8_t magic[3] = { 200, 100, 80 };
    for (int i = 0; i < 3; i++) {
        mouse_write(0xF3); /* set sample rate */
        mouse_read();
        mouse_write(magic[i]);
        mouse_read();
    }
    mouse_write(0xF2); /* get device ID */
    mouse_read();
    if (mouse_read() == 3) packet_size = 4;
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
    negotiate_wheel();
    mouse_write(0xF4); /* enable data reporting */
    mouse_read();       /* ACK */

    register_interrupt_handler(44, mouse_handler); /* IRQ12 -> remapped vector 44 */
    pic_unmask_irq(2);  /* cascade line - the slave PIC can't interrupt without it */
    pic_unmask_irq(12);
}

void mouse_set_bounds(int width, int height) {
    bound_w = width > 0 ? width : 1;
    bound_h = height > 0 ? height : 1;
    cursor_x = bound_w / 2;
    cursor_y = bound_h / 2;
}

int mouse_poll_event(mouse_event_t* out) {
    if (q_head == q_tail) return 0;
    mouse_event_t ev = queue[q_tail];
    q_tail = (q_tail + 1) % MOUSE_QUEUE_SIZE;
    if (out) *out = ev;
    return 1;
}

int mouse_x(void) { return cursor_x; }
int mouse_y(void) { return cursor_y; }
uint8_t mouse_buttons(void) { return button_state; }
