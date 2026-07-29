#ifndef KEYBOARD_H
#define KEYBOARD_H

/* Registers the IRQ1 handler that translates PS/2 scancode set 1 into
 * ASCII and buffers it for keyboard_getchar(). */
void keyboard_install(void);

/* Returns 1 if there's buffered input waiting, 0 otherwise. */
int keyboard_haschar(void);

/* Blocks (halting the CPU between interrupts) until a character is
 * available, then returns it. '\b' is reported for backspace. */
char keyboard_getchar(void);

#endif
