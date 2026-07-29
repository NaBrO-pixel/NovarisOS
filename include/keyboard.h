#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdint.h>

/* Registers the IRQ1 handler that translates PS/2 scancode set 1 into
 * ASCII and buffers it for keyboard_getchar(). */
void keyboard_install(void);

/* Returns 1 if there's buffered input waiting, 0 otherwise. */
int keyboard_haschar(void);

/* Blocks (halting the CPU between interrupts) until a character is
 * available, then returns it. '\b' is reported for backspace. */
char keyboard_getchar(void);

/* --- key events -------------------------------------------------------- */
/*
 * The ASCII stream above is all a line-oriented shell ever needed. A
 * desktop needs more: which modifiers are held (so Command-W can close a
 * window), the arrow keys and Escape (which have no ASCII at all), and
 * press and release as separate facts. So the driver publishes a second,
 * richer queue alongside the first - they coexist, and the VGA-text
 * fallback shell keeps using the simple one.
 */

#define KEY_MOD_SHIFT 0x01
#define KEY_MOD_CTRL  0x02
#define KEY_MOD_ALT   0x04  /* doubles as Command - see below */
#define KEY_MOD_SUPER 0x08  /* the physical Windows/Command key */
#define KEY_MOD_CAPS  0x10

/* Novaris treats Alt and Super interchangeably as "Command", because on a
 * PC keyboard Alt sits roughly where a Mac keyboard's Command key does,
 * and a Super keypress doesn't always reach the guest at all. */
#define KEY_MOD_COMMAND (KEY_MOD_ALT | KEY_MOD_SUPER)

/* Keys with no ASCII of their own get codes above 255. */
enum {
    KEY_LEFT = 0x100, KEY_RIGHT, KEY_UP, KEY_DOWN,
    KEY_HOME, KEY_END, KEY_PAGE_UP, KEY_PAGE_DOWN, KEY_DELETE, KEY_INSERT,
    KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6,
    KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12,
    KEY_LSHIFT, KEY_RSHIFT, KEY_LCTRL, KEY_RCTRL, KEY_LALT, KEY_RALT,
    KEY_SUPER, KEY_CAPSLOCK,
};

#define KEY_ESCAPE    27
#define KEY_TAB       '\t'
#define KEY_ENTER     '\n'
#define KEY_BACKSPACE '\b'

typedef struct {
    uint16_t code;    /* ASCII value, or one of the KEY_* codes above */
    uint8_t  ascii;   /* the printable character, or 0 */
    uint8_t  mods;    /* KEY_MOD_* bitmask at the time of the event */
    uint8_t  pressed; /* 1 = key down, 0 = key up */
} key_event_t;

/* Dequeues one key event; returns 0 when the queue is empty. */
int keyboard_poll_event(key_event_t* out);

/* The modifier keys currently held down. */
uint8_t keyboard_mods(void);

#endif
