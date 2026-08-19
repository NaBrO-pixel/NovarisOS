#ifndef INPUT64_H
#define INPUT64_H

#include <stdint.h>

/* Keyboard and mouse, as Linux input devices (Milestone 70).
 *
 * The 32-bit tree has both drivers and neither was ported, so the
 * 64-bit tree gained a display in Milestone 67 and had nothing to type
 * at it with. This is the other half.
 *
 * The interface is the argument, not the PS/2 decoding. A driver only
 * the kernel can read is not an input driver, and the shape Wine sits
 * on top of on Linux is evdev: character devices under /dev/input that
 * hand back a stream of fixed-size records. So these are device nodes
 * in the filesystem, read with read(2), returning Linux's own
 * `struct input_event` at Linux's own layout - exactly the argument
 * Milestone 67 made for /dev/fb0 and the fbdev ioctls.
 */

/* struct input_event, x86-64. 24 bytes:
 *   0  struct timeval time   (two 64-bit words)
 *   16 __u16 type
 *   18 __u16 code
 *   20 __s32 value
 * The layout is the kernel's and must not be tidied - a program reads
 * this with a struct definition that came from Linux's headers. */
typedef struct {
    uint64_t tv_sec;
    uint64_t tv_usec;
    uint16_t type;
    uint16_t code;
    int32_t  value;
} input64_event_t;

/* Event types. */
#define EV64_SYN 0x00
#define EV64_KEY 0x01
#define EV64_REL 0x02

/* Codes. Linux's keycodes for the main block are AT scancode set 1
 * unchanged - KEY_ESC is 1 and a set-1 escape is 0x01 - which is not a
 * coincidence and is why translation here is a table only for the keys
 * that moved. */
#define REL64_X 0x00
#define REL64_Y 0x01

#define BTN64_LEFT   0x110
#define BTN64_RIGHT  0x111
#define BTN64_MIDDLE 0x112

#define SYN64_REPORT 0

/* Which device a read is for. */
#define INPUT64_KBD   0
#define INPUT64_MOUSE 1

/* Registers the IRQ1 and IRQ12 handlers, sets the PS/2 controller up for
 * a mouse, and unmasks both lines. */
void input64_install(void);

/* Copies whole events into `buf`, never a partial one - a caller that
 * asked for 30 bytes gets 24 and one event, because half a record is
 * not something evdev ever returns. Returns bytes written; 0 when
 * nothing is queued (this does not block). */
uint64_t input64_read(int device, void* buf, uint64_t len);

/* How many events are waiting. */
uint64_t input64_queued(int device);

/* Interrupt counts, so a test can tell "the driver decoded nothing"
 * from "the line never fired". */
uint64_t input64_irqs(int device);

/* Feeds one byte in as though it had come from the controller. This is
 * how the decoder is tested without a keyboard: the port read is the
 * only part that needs real hardware, and it is deliberately the only
 * part this does not do. */
void input64_feed_scancode(uint8_t byte);
void input64_feed_mouse_byte(uint8_t byte);

/* The modifier state the driver is tracking, for tests. */
#define INPUT64_MOD_SHIFT 0x01
#define INPUT64_MOD_CTRL  0x02
#define INPUT64_MOD_ALT   0x04
uint32_t input64_modifiers(void);

void input64_reset(void);

/* Creates /dev/input/event0 and event1. Needs the filesystem. */
int  input64_register(void);

#endif
