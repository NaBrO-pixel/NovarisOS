#include "framebuffer.h"
#include "serial.h"

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

/* --- who is allowed to write here (Milestone 36) -------------------------
 *
 * Once the desktop is up the framebuffer belongs to the compositor: every
 * frame is built in an off-screen surface and pushed out one damaged
 * rectangle at a time by fb_blit(). Anything else that writes here writes
 * *behind* that, and what it draws survives in every rectangle the
 * compositor does not happen to repaint - which is a stripe of stale
 * pixels sitting on the screen for ever.
 *
 * That is not hypothetical: a black band sixteen pixels tall appeared at
 * a fixed place during Wine runs and stayed there, and finding out who
 * drew it took a day of wrong guesses. So the framebuffer says so itself
 * now, once, naming the call. Cheap, and it turns "there is a mark on the
 * screen" into a line of text.
 *
 * fb_blit() is the compositor's own path and is deliberately not
 * reported. */
static int fb_owned_by_compositor = 0;
static int fb_trespass_reported = 0;

void fb_set_compositor_owned(int owned) {
    fb_owned_by_compositor = owned ? 1 : 0;
}

static void fb_trespass(const char* who) {
    if (!fb_owned_by_compositor || fb_trespass_reported) return;
    fb_trespass_reported = 1;
    serial_writestring("[fb] ");
    serial_writestring(who);
    serial_writestring(" wrote to the framebuffer while the compositor "
                       "owned it - see kernel/framebuffer.c\n");
}

void fb_put_pixel(uint32_t x, uint32_t y, uint32_t color) {
    fb_trespass("fb_put_pixel");
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
    fb_trespass("fb_fill_rect");
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

void fb_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
             const uint32_t* src, uint32_t src_pitch) {
    if (!fb_base || x >= fb_w || y >= fb_h) return;
    if (x + w > fb_w) w = fb_w - x;
    if (y + h > fb_h) h = fb_h - y;

    for (uint32_t row = 0; row < h; row++) {
        const uint32_t* s = src + (uint32_t)row * src_pitch;
        uint8_t* d = fb_base + (y + row) * fb_pitch + x * fb_bytes_per_pixel;
        if (fb_bytes_per_pixel == 4) {
            /* The common case: our surfaces are already 0x00RRGGBB, which
             * is exactly what a 32bpp linear framebuffer wants, so this is
             * a straight 32-bit copy per row. */
            uint32_t* d32 = (uint32_t*)d;
            for (uint32_t i = 0; i < w; i++) d32[i] = s[i] & 0x00FFFFFFu;
        } else {
            for (uint32_t i = 0; i < w; i++) {
                uint32_t c = s[i];
                d[0] = (uint8_t)(c & 0xFF);
                d[1] = (uint8_t)((c >> 8) & 0xFF);
                d[2] = (uint8_t)((c >> 16) & 0xFF);
                d += 3;
            }
        }
    }
}

void fb_scroll_up(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t dy,
                   uint32_t bg_color) {
    fb_trespass("fb_scroll_up");
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
