#ifndef FB64_H
#define FB64_H

#include <stdint.h>
#include "multiboot.h"

/* The linear framebuffer, on the 64-bit side.
 *
 * boot64.s has asked for a 1024x768x32 linear graphics mode since it was
 * written - the VIDMODE bit in its Multiboot header - and GRUB has been
 * granting it ever since. Nothing read the answer until Milestone 67, so
 * the 64-bit kernel booted onto a working framebuffer twenty-two
 * milestones running and drew nothing on it.
 *
 * This is the 32-bit tree's framebuffer.c rewritten rather than shared:
 * that one takes a uint32_t address and writes through the identity map,
 * neither of which survives the move to long mode. What is kept is its
 * pixel packing, which is correct and was worth not re-deriving. */

#define FB64_OK          0
#define FB64_NO_MODE    -1   /* the bootloader gave us no framebuffer */
#define FB64_NOT_RGB    -2   /* EGA text or indexed colour, not a linear LFB */
#define FB64_BAD_DEPTH  -3   /* a depth this driver cannot pack pixels for */
#define FB64_NO_MAP     -4   /* the framebuffer could not be mapped */

/* Reads the geometry the bootloader granted - not the one that was
 * requested - and maps the framebuffer. Safe to call when there is no
 * framebuffer at all: it returns FB64_NO_MODE and leaves fb64_ready()
 * false, so a headless boot is a report rather than a crash. */
int fb64_init(const multiboot_info_t* mbi);

/* Creates /dev/fb0 and marks it as this device. Call after ramfs64_init.
 * Returns 0 if there is no framebuffer or the node could not be made. */
int      fb64_register(void);

int      fb64_ready(void);
uint32_t fb64_width(void);
uint32_t fb64_height(void);
uint32_t fb64_pitch(void);        /* bytes per scanline, not pixels */
uint32_t fb64_bpp(void);
uint64_t fb64_phys(void);         /* what a process would mmap */
uint64_t fb64_bytes(void);        /* pitch * height, rounded to whole pages */

/* Packs 8-bit components the way this framebuffer's pixels are laid out. */
uint32_t fb64_rgb(uint8_t r, uint8_t g, uint8_t b);

void     fb64_put_pixel(uint32_t x, uint32_t y, uint32_t color);
uint32_t fb64_get_pixel(uint32_t x, uint32_t y);
void     fb64_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                        uint32_t color);
void     fb64_clear(uint32_t color);

/* Pushes a rectangle of 0x00RRGGBB pixels, `src_pitch` pixels per row.
 * The one path anything above this file should use to reach VRAM. */
void     fb64_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                   const uint32_t* src, uint32_t src_pitch);

/* How many pixels have been written, ever. The bring-up test uses it to
 * tell "drew nothing" from "drew something invisible". */
uint64_t fb64_pixels_written(void);

#endif
