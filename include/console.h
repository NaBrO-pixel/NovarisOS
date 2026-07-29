#ifndef CONSOLE_H
#define CONSOLE_H

#include <stdint.h>
#include <stddef.h>

/* Standard VGA 16-color palette. Kept as the public color API (every
 * existing terminal_writestring_color(..., VGA_COLOR_*) call site in
 * kernel.c/shell.c is unchanged) - console.c maps these onto real RGB
 * values when drawing to the framebuffer, or onto the original VGA
 * attribute byte when falling back to text mode. */
enum vga_color {
    VGA_COLOR_BLACK = 0,
    VGA_COLOR_BLUE = 1,
    VGA_COLOR_GREEN = 2,
    VGA_COLOR_CYAN = 3,
    VGA_COLOR_RED = 4,
    VGA_COLOR_MAGENTA = 5,
    VGA_COLOR_BROWN = 6,
    VGA_COLOR_LIGHT_GREY = 7,
    VGA_COLOR_DARK_GREY = 8,
    VGA_COLOR_LIGHT_BLUE = 9,
    VGA_COLOR_LIGHT_GREEN = 10,
    VGA_COLOR_LIGHT_CYAN = 11,
    VGA_COLOR_LIGHT_RED = 12,
    VGA_COLOR_LIGHT_MAGENTA = 13,
    VGA_COLOR_LIGHT_BROWN = 14,
    VGA_COLOR_WHITE = 15,
};

/* Tries to bring up the framebuffer-backed console (desktop background +
 * a bordered "terminal window" the text renders inside). Call this
 * before terminal_initialize() if the bootloader reported framebuffer
 * info (MULTIBOOT_INFO_FRAMEBUFFER) - pass the *granted* geometry, which
 * may differ from what boot.s's Multiboot header requested. addr must
 * already be mapped. Returns 1 on success; on failure (unsupported pixel
 * format, or simply never called), terminal_initialize() falls back to
 * VGA text mode automatically. */
int console_use_framebuffer(uint32_t addr, uint32_t pitch, uint32_t width,
                             uint32_t height, uint8_t bpp);

/* True once console_use_framebuffer() has succeeded - lets other
 * graphical drivers (the mouse cursor) know whether there's actually a
 * framebuffer to draw on. */
int console_has_framebuffer(void);

void terminal_initialize(void);
void terminal_setcolor(uint8_t color);
uint8_t terminal_getcolor(void);
void terminal_putchar(char c);
void terminal_write(const char* data, size_t size);
void terminal_writestring(const char* data);
void terminal_writestring_color(const char* data, uint8_t color);
void terminal_backspace(void);

/* --- redirecting the console into a window ----------------------------- */
/*
 * Once the desktop is up, "the console" is a Terminal window, not the
 * whole screen. Rather than teach every terminal_writestring() call site
 * in the kernel about windows, the console grows a sink: a callback that
 * receives each character plus the color it was written in. With one
 * installed, nothing is drawn directly to the framebuffer any more - but
 * the COM1 mirror still happens first, so the serial transcript
 * tools/qemu_test.py asserts against is byte-for-byte unchanged.
 */

typedef struct {
    char c;
    uint8_t color;  /* VGA attribute byte: fg in the low nibble */
} console_cell_t;

typedef void (*console_sink_t)(char c, uint8_t color);

/* Passing 0 restores direct framebuffer/VGA drawing. */
void console_set_sink(console_sink_t sink);

/* Clears whatever the console currently is: the framebuffer chrome, the
 * text screen, or the Terminal window's grid. */
void console_clear(void);

/* Everything written before a sink was installed, so the Terminal window
 * can open showing the kernel's own boot log rather than a blank screen.
 * Returns a console_cell_t array. */
const console_cell_t* console_boot_log(uint32_t* out_len);

/* The 16-color VGA palette entry as a real RGB value, so a window can
 * render colored console text with the same palette. */
uint32_t console_vga_rgb(uint8_t index);

#endif
