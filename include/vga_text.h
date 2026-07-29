#ifndef VGA_TEXT_H
#define VGA_TEXT_H

#include <stdint.h>
#include <stddef.h>

/* The original Milestone 1/2 VGA text-mode driver, kept as a fallback
 * for when the bootloader can't grant a linear framebuffer (see
 * console.c, which is the API everything else in the kernel actually
 * calls now). Real hardware without VBE support, or a Multiboot
 * bootloader that ignores the video-mode request, still gets a working
 * console this way instead of a blank screen. */

void vga_text_initialize(void);
void vga_text_setcolor(uint8_t color);
void vga_text_putchar(char c);
void vga_text_write(const char* data, size_t size);
void vga_text_writestring(const char* data);
void vga_text_writestring_color(const char* data, uint8_t color);
void vga_text_backspace(void);

#endif
