#ifndef MOUSE_H
#define MOUSE_H

/* Enables the i8042 auxiliary (mouse) port, registers the IRQ12 handler,
 * and draws an initial cursor if a framebuffer console is active (see
 * console_has_framebuffer()). Safe to call even without a framebuffer -
 * the hardware still gets initialized and packets still get parsed, just
 * with no visible cursor to move. */
void mouse_install(void);

#endif
