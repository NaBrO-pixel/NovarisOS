#include "framebuffer.h"

static uint8_t* fb_base = 0;
static uint32_t fb_pitch = 0;   /* bytes per scanline */
static uint32_t fb_w = 0, fb_h = 0;
static uint8_t fb_bytes_per_pixel = 0;

int fb_init(uint32_t addr, uint32_t pitch, uint32_t width, uint32_t height,
            uint8_t bpp) {
    if (bpp != 32 && bpp != 24) return 0; /* only RGB modes we know how to pack */

    fb_base = (uint8_t*)addr;
    fb_pitch = pitch;
    fb_w = width;
    fb_h = height;
    fb_bytes_per_pixel = (uint8_t)(bpp / 8);
    return 1;
}

uint32_t fb_width(void) { return fb_w; }
uint32_t fb_height(void) { return fb_h; }

uint32_t fb_rgb(uint8_t r, uint8_t g, uint8_t b) {
    /* Both 24 and 32bpp VBE linear-framebuffer modes we're expecting are
     * plain packed 0x00RRGGBB in memory byte order B,G,R[,pad] on
     * little-endian x86 - so a 32-bit store of this value does the right
     * thing either way (the 4th byte is simply unused padding at 24bpp,
     * handled per-pixel in fb_put_pixel). */
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

void fb_put_pixel(uint32_t x, uint32_t y, uint32_t color) {
    if (x >= fb_w || y >= fb_h) return;
    uint8_t* p = fb_base + y * fb_pitch + x * fb_bytes_per_pixel;
    p[0] = (uint8_t)(color & 0xFF);         /* B */
    p[1] = (uint8_t)((color >> 8) & 0xFF);  /* G */
    p[2] = (uint8_t)((color >> 16) & 0xFF); /* R */
    if (fb_bytes_per_pixel == 4) p[3] = 0;
}

uint32_t fb_get_pixel(uint32_t x, uint32_t y) {
    if (x >= fb_w || y >= fb_h) return 0;
    uint8_t* p = fb_base + y * fb_pitch + x * fb_bytes_per_pixel;
    return fb_rgb(p[2], p[1], p[0]);
}

void fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
    uint32_t x_end = x + w;
    uint32_t y_end = y + h;
    if (x_end > fb_w) x_end = fb_w;
    if (y_end > fb_h) y_end = fb_h;

    for (uint32_t yy = y; yy < y_end; yy++) {
        for (uint32_t xx = x; xx < x_end; xx++) {
            fb_put_pixel(xx, yy, color);
        }
    }
}

void fb_scroll_up(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t dy,
                   uint32_t bg_color) {
    if (dy >= h) {
        fb_fill_rect(x, y, w, h, bg_color);
        return;
    }

    uint32_t x_end = x + w;
    if (x_end > fb_w) x_end = fb_w;
    uint32_t row_bytes = (x_end - x) * fb_bytes_per_pixel;

    for (uint32_t row = 0; row < h - dy; row++) {
        if (y + row >= fb_h) break;
        uint8_t* dst = fb_base + (y + row) * fb_pitch + x * fb_bytes_per_pixel;
        uint8_t* src = fb_base + (y + row + dy) * fb_pitch + x * fb_bytes_per_pixel;
        for (uint32_t b = 0; b < row_bytes; b++) dst[b] = src[b];
    }

    fb_fill_rect(x, y + h - dy, w, dy, bg_color);
}
