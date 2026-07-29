#ifndef FRAMEBUFFER_H
#define FRAMEBUFFER_H

#include <stdint.h>

/* Initializes the framebuffer driver from the address/geometry GRUB
 * actually granted (see MULTIBOOT_INFO_FRAMEBUFFER in kernel_main) -
 * not necessarily what boot.s's Multiboot header requested. addr must
 * already be mapped before any drawing happens (kernel_main maps it via
 * paging_map_page before calling this). Only 32bpp and 24bpp RGB are
 * supported; anything else fails loudly rather than drawing garbage. */
int fb_init(uint32_t addr, uint32_t pitch, uint32_t width, uint32_t height,
            uint8_t bpp);

uint32_t fb_width(void);
uint32_t fb_height(void);

/* Packs 8-bit components into whatever pixel format fb_init() detected. */
uint32_t fb_rgb(uint8_t r, uint8_t g, uint8_t b);

void fb_put_pixel(uint32_t x, uint32_t y, uint32_t color);
void fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);

/* Reads a pixel back - used by the mouse cursor to save/restore
 * whatever was underneath it before drawing at a new position. */
uint32_t fb_get_pixel(uint32_t x, uint32_t y);

/* Pushes a rectangle of 32-bit pixels (0x00RRGGBB, `src_pitch` pixels per
 * row) to the framebuffer at (x, y), converting to the hardware's pixel
 * size. This is the one path the compositor uses to reach VRAM - see
 * kernel/gfx.c, which assembles a whole frame off-screen first. */
void fb_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
             const uint32_t* src, uint32_t src_pitch);

/* Shifts a rectangular region up by `dy` pixel rows (for text scrolling)
 * and fills the newly-exposed strip at the bottom with bg_color. */
void fb_scroll_up(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t dy,
                   uint32_t bg_color);

#endif
