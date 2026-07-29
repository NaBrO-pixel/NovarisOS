#ifndef MOUSE_H
#define MOUSE_H

#include <stdint.h>

#define MOUSE_BUTTON_LEFT   0x01
#define MOUSE_BUTTON_RIGHT  0x02
#define MOUSE_BUTTON_MIDDLE 0x04

typedef struct {
    int16_t x, y;       /* cursor position after this event, in pixels */
    int8_t  dx, dy;     /* movement this packet reported */
    int8_t  wheel;      /* scroll wheel: +1 up, -1 down, 0 none */
    uint8_t buttons;    /* MOUSE_BUTTON_* currently held */
    uint8_t pressed;    /* buttons that went down in this event */
    uint8_t released;   /* buttons that came up in this event */
} mouse_event_t;

/* Enables the i8042 auxiliary (mouse) port, negotiates the scroll-wheel
 * protocol if the device supports it, and registers the IRQ12 handler.
 * Safe to call with no framebuffer - packets are still parsed, there's
 * just nothing drawing a cursor. */
void mouse_install(void);

/* Confines the cursor to a box of this size and recenters it. The
 * desktop sets it to the screen size once the framebuffer is up. */
void mouse_set_bounds(int width, int height);

/* Dequeues one event; returns 0 when the queue is empty.
 *
 * Unlike the Milestone 7 driver, nothing in here draws. The cursor is a
 * layer the compositor paints last, which is what lets windows move
 * underneath it - the old "save and restore the pixels behind the
 * pointer" trick has no idea the pixels it saved have gone stale. */
int mouse_poll_event(mouse_event_t* out);

int     mouse_x(void);
int     mouse_y(void);
uint8_t mouse_buttons(void);

#endif
