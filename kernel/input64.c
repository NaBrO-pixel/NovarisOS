/* input64.c - a PS/2 keyboard and mouse, published as Linux input devices.
 *
 * Two IRQs and one controller. The keyboard is IRQ1 and needs no setup
 * at all - the BIOS left it enabled and streaming. The mouse is IRQ12
 * and needs a good deal, because the PS/2 controller powers up with the
 * auxiliary port disabled and the mouse itself powers up not reporting.
 *
 * The decoding is the small half. The interface is the point: see
 * input64.h.
 */

#include "input64.h"
#include "io.h"
#include "idt64.h"
#include "ramfs64.h"

#define PS2_DATA   0x60
#define PS2_STATUS 0x64
#define PS2_CMD    0x64

/* Status register bits. */
#define PS2_ST_OUTPUT_FULL 0x01     /* there is a byte to read       */
#define PS2_ST_INPUT_FULL  0x02     /* the controller is still busy  */
#define PS2_ST_FROM_AUX    0x20     /* the byte came from the mouse  */

/* Controller commands. */
#define PS2_CMD_ENABLE_AUX   0xA8
#define PS2_CMD_READ_CONFIG  0x20
#define PS2_CMD_WRITE_CONFIG 0x60
#define PS2_CMD_TO_AUX       0xD4

/* Mouse commands. */
#define MOUSE_SET_DEFAULTS  0xF6
#define MOUSE_ENABLE_REPORT 0xF4
#define MOUSE_ACK           0xFA

#define QUEUE_MAX 256

typedef struct {
    input64_event_t ev[QUEUE_MAX];
    uint64_t head, tail;          /* head is written, tail is read */
    uint64_t dropped;
    uint64_t irqs;
} queue_t;

static queue_t queues[2];
static uint32_t modifiers;

/* A monotonic stand-in for the wall clock. Every event carries a
 * timestamp because struct input_event has the field; nothing here has
 * a real clock yet, and a counter that only ever increases is a more
 * honest answer than a zero that claims to be a time. */
static uint64_t event_seq;

static void push(int dev, uint16_t type, uint16_t code, int32_t value) {
    queue_t* q = &queues[dev];
    uint64_t next = (q->head + 1) % QUEUE_MAX;

    if (next == q->tail) { q->dropped++; return; }   /* full: drop the newest */

    q->ev[q->head].tv_sec  = event_seq / 1000000;
    q->ev[q->head].tv_usec = event_seq % 1000000;
    q->ev[q->head].type    = type;
    q->ev[q->head].code    = code;
    q->ev[q->head].value   = value;
    q->head = next;
    event_seq++;
}

/* Every packet of related events ends with a SYN_REPORT. A reader that
 * did not see one would not know whether an X movement and a Y movement
 * were one gesture or two, which is the whole reason evdev has it. */
static void sync_report(int dev) {
    push(dev, EV64_SYN, SYN64_REPORT, 0);
}

/* --- the keyboard --------------------------------------------------- */

/* Set 1 makes a key by sending its scancode and breaks it by sending
 * the same code with the top bit set. Linux's keycodes for this block
 * are the same numbers, so the code *is* the keycode; only the modifier
 * bookkeeping needs to know which is which. */
#define SC_LSHIFT 0x2A
#define SC_RSHIFT 0x36
#define SC_CTRL   0x1D
#define SC_ALT    0x38

static int kbd_extended;        /* an 0xE0 prefix is pending */

void input64_feed_scancode(uint8_t byte) {
    uint8_t code;
    int released;

    if (byte == 0xE0) { kbd_extended = 1; return; }

    released = (byte & 0x80) != 0;
    code     = byte & 0x7F;

    /* The extended set repeats the base codes with a prefix - the right
     * Ctrl is E0 1D, the same 1D as the left one. Linux gives those
     * their own keycodes well above the base block; this driver reports
     * the base keycode and does not pretend to tell the two apart,
     * which is a limitation and not a lie as long as it is written
     * down. */
    kbd_extended = 0;

    switch (code) {
    case SC_LSHIFT:
    case SC_RSHIFT:
        if (released) modifiers &= ~INPUT64_MOD_SHIFT;
        else          modifiers |=  INPUT64_MOD_SHIFT;
        break;
    case SC_CTRL:
        if (released) modifiers &= ~INPUT64_MOD_CTRL;
        else          modifiers |=  INPUT64_MOD_CTRL;
        break;
    case SC_ALT:
        if (released) modifiers &= ~INPUT64_MOD_ALT;
        else          modifiers |=  INPUT64_MOD_ALT;
        break;
    default:
        break;
    }

    /* value: 0 release, 1 press. Linux also has 2 for auto-repeat,
     * which the hardware delivers as a stream of presses and this
     * driver passes on as presses - a reader counting keystrokes would
     * over-count a held key, exactly as it would on a kernel that had
     * not been told to synthesise repeats either. */
    push(INPUT64_KBD, EV64_KEY, code, released ? 0 : 1);
    sync_report(INPUT64_KBD);
}

static void keyboard_irq(registers64_t* r) {
    (void)r;
    queues[INPUT64_KBD].irqs++;

    /* Drain, rather than reading one byte. The controller can have more
     * than one waiting by the time the handler runs, and a byte left
     * behind means the next interrupt decodes a prefix as a key. */
    while (inb(PS2_STATUS) & PS2_ST_OUTPUT_FULL) {
        uint8_t st = inb(PS2_STATUS);
        uint8_t b;

        if (st & PS2_ST_FROM_AUX) break;    /* the mouse's, not ours */
        b = inb(PS2_DATA);
        input64_feed_scancode(b);
    }
}

/* --- the mouse ------------------------------------------------------- */

/* The standard PS/2 mouse sends three bytes: flags, then a signed X and
 * Y as 9-bit values whose sign and overflow bits live in the flags. */
static uint8_t mouse_packet[3];
static int     mouse_index;
static uint8_t mouse_buttons;

void input64_feed_mouse_byte(uint8_t byte) {
    /* What a first byte has to look like, or the stream is out of phase
     * and the byte is dropped rather than shifted in - so the packet
     * boundary re-syncs instead of staying wrong forever.
     *
     * Bit 3 is always set on a real packet, and the two overflow bits
     * are never both meaningful in one: a mouse that has genuinely
     * overflowed has nothing useful to report anyway, so treating that
     * as noise costs nothing and rejects a great deal of it. This
     * matters because the mouse answers every command with 0xFA, and
     * 0xFA has bit 3 set - checking bit 3 alone accepts an ACK as a
     * first byte and mis-frames every packet after it. */
    if (mouse_index == 0 && ((byte & 0x08) == 0 || (byte & 0xC0) != 0))
        return;

    mouse_packet[mouse_index++] = byte;
    if (mouse_index < 3) return;
    mouse_index = 0;

    {
        uint8_t flags = mouse_packet[0];
        int32_t dx = mouse_packet[1];
        int32_t dy = mouse_packet[2];
        uint8_t now = flags & 0x07;
        uint8_t changed = now ^ mouse_buttons;
        int moved = 0;

        if (flags & 0x10) dx |= ~0xFF;        /* sign-extend from 9 bits */
        if (flags & 0x20) dy |= ~0xFF;

        if (changed & 0x01)
            push(INPUT64_MOUSE, EV64_KEY, BTN64_LEFT,   (now & 0x01) ? 1 : 0);
        if (changed & 0x02)
            push(INPUT64_MOUSE, EV64_KEY, BTN64_RIGHT,  (now & 0x02) ? 1 : 0);
        if (changed & 0x04)
            push(INPUT64_MOUSE, EV64_KEY, BTN64_MIDDLE, (now & 0x04) ? 1 : 0);
        mouse_buttons = now;

        if (dx) { push(INPUT64_MOUSE, EV64_REL, REL64_X, dx); moved = 1; }
        /* The mouse counts Y upwards and every screen counts it
         * downwards. Negating here rather than in each reader is the
         * same choice Linux makes. */
        if (dy) { push(INPUT64_MOUSE, EV64_REL, REL64_Y, -dy); moved = 1; }

        if (changed || moved) sync_report(INPUT64_MOUSE);
    }
}

static void mouse_irq(registers64_t* r) {
    (void)r;
    queues[INPUT64_MOUSE].irqs++;

    while (inb(PS2_STATUS) & PS2_ST_OUTPUT_FULL) {
        uint8_t st = inb(PS2_STATUS);
        uint8_t b;

        if (!(st & PS2_ST_FROM_AUX)) break;      /* the keyboard's */
        b = inb(PS2_DATA);
        input64_feed_mouse_byte(b);
    }
}

/* --- controller setup ------------------------------------------------ */

/* Bounded, because a controller that never clears the bit would
 * otherwise hang the boot - and on a machine with no PS/2 controller at
 * all, every one of these spins forever. */
static void wait_writable(void) {
    for (int i = 0; i < 100000; i++)
        if (!(inb(PS2_STATUS) & PS2_ST_INPUT_FULL)) return;
}

static void wait_readable(void) {
    for (int i = 0; i < 100000; i++)
        if (inb(PS2_STATUS) & PS2_ST_OUTPUT_FULL) return;
}

static void controller_write(uint8_t cmd) {
    wait_writable();
    outb(PS2_CMD, cmd);
}

static void mouse_write(uint8_t byte) {
    controller_write(PS2_CMD_TO_AUX);
    wait_writable();
    outb(PS2_DATA, byte);
    /* The mouse answers every command with an ACK. Reading it here is
     * what keeps it out of the packet stream - an unread ACK arrives at
     * the IRQ handler as a first byte with bit 3 clear, and the
     * re-sync above exists because getting this wrong is easy. */
    wait_readable();
    (void)inb(PS2_DATA);
}

void input64_reset(void) {
    for (int d = 0; d < 2; d++) {
        queues[d].head = queues[d].tail = 0;
        queues[d].dropped = 0;
        queues[d].irqs = 0;
    }
    modifiers     = 0;
    kbd_extended  = 0;
    mouse_index   = 0;
    mouse_buttons = 0;
}

void input64_install(void) {
    uint8_t config;

    input64_reset();

    register_interrupt_handler64(33, keyboard_irq);   /* IRQ1  */
    register_interrupt_handler64(44, mouse_irq);      /* IRQ12 */

    /* The auxiliary port, off at power-up. */
    controller_write(PS2_CMD_ENABLE_AUX);

    /* Turn the mouse's interrupt on in the controller's configuration
     * byte. Bit 1 is IRQ12; bit 5 disables the aux clock and has to go
     * off, because enabling the port and leaving its clock disabled is
     * a mouse that is present, configured and silent. */
    controller_write(PS2_CMD_READ_CONFIG);
    wait_readable();
    config = inb(PS2_DATA);
    config |=  (1 << 1);
    config &= ~(1 << 5);
    controller_write(PS2_CMD_WRITE_CONFIG);
    wait_writable();
    outb(PS2_DATA, config);

    mouse_write(MOUSE_SET_DEFAULTS);
    mouse_write(MOUSE_ENABLE_REPORT);

    idt64_irq_set_mask(1, 0);
    idt64_irq_set_mask(12, 0);
    /* IRQ12 is on the second PIC, which reaches the CPU through line 2
     * of the first. A masked cascade is a mouse that is enabled at both
     * ends and still never interrupts. */
    idt64_irq_set_mask(2, 0);
}

/* --- reading --------------------------------------------------------- */

uint64_t input64_read(int device, void* buf, uint64_t len) {
    uint8_t* out = (uint8_t*)buf;
    uint64_t written = 0;
    queue_t* q;

    if (device < 0 || device > 1 || !buf) return 0;
    q = &queues[device];

    /* Whole events only. */
    while (q->tail != q->head && written + sizeof(input64_event_t) <= len) {
        const uint8_t* src = (const uint8_t*)&q->ev[q->tail];
        for (uint64_t i = 0; i < sizeof(input64_event_t); i++)
            out[written + i] = src[i];
        written += sizeof(input64_event_t);
        q->tail = (q->tail + 1) % QUEUE_MAX;
    }
    return written;
}

uint64_t input64_queued(int device) {
    const queue_t* q;
    if (device < 0 || device > 1) return 0;
    q = &queues[device];
    return (q->head + QUEUE_MAX - q->tail) % QUEUE_MAX;
}

uint64_t input64_irqs(int device) {
    if (device < 0 || device > 1) return 0;
    return queues[device].irqs;
}

uint32_t input64_modifiers(void) { return modifiers; }

/* Publishes the two devices as /dev/input/event0 and event1, once the
 * filesystem exists - the same split as fb64_register, and for the same
 * reason: the driver comes up with the interrupt controller and the
 * filesystem several layers later.
 *
 * The names are Linux's. A program that opens /dev/input/event0 and
 * reads 24-byte records off it is doing exactly what it would do on
 * Linux, which is the whole point of publishing them this way rather
 * than inventing a call. */
int input64_register(void) {
    int node;

    if (ramfs64_mkdirp("/dev/input") < 0) return 0;

    node = ramfs64_create("/dev/input/event0", 0);
    if (node < 0) node = ramfs64_lookup("/dev/input/event0");
    if (node < 0 || !ramfs64_set_device(node, RAMFS64_DEV_KBD)) return 0;

    node = ramfs64_create("/dev/input/event1", 0);
    if (node < 0) node = ramfs64_lookup("/dev/input/event1");
    if (node < 0 || !ramfs64_set_device(node, RAMFS64_DEV_MOUSE)) return 0;

    return 1;
}
