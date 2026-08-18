/* fb64.c - the linear framebuffer.
 *
 * Two things about this file are consequences of Milestone 66 rather
 * than decisions taken here.
 *
 * The framebuffer is at 0xFD000000 and RAM stops at 0x7FFE0000, so VRAM
 * is not merely above the old 1GB boot window - it is above RAM
 * entirely. Before the direct map there was no mechanism in this kernel
 * for reaching a physical address that the boot window did not cover,
 * which is why the 64-bit tree had no display: not because nobody wrote
 * one, but because there was nowhere to map it.
 *
 * And the mapping is uncacheable, which makes every store go straight to
 * the device. That is correct and slow. The fix is write-combining via
 * PAT, which is a real piece of work (a MSR, a TLB flush protocol, and a
 * decision about what to do on a machine that lacks it) and is
 * deliberately not in this milestone. Anything drawing whole frames
 * should assemble them in RAM and push them with one fb64_blit, which is
 * why that is the only path offered above put_pixel.
 */

#include "fb64.h"
#include "paging64.h"
#include "serial64.h"
#include "ramfs64.h"

static uint8_t* base;              /* VRAM, through the MMIO mapping */
static uint64_t phys_addr;
static uint32_t pitch;             /* bytes per scanline */
static uint32_t width, height;
static uint32_t bytes_per_pixel;
static uint32_t depth;
static uint64_t mapped_bytes;
static uint64_t pixels_written;
static int      ready;

#define HUGE_MASK 0x1FFFFFULL

int fb64_init(const multiboot_info_t* mbi) {
    uint64_t span, aligned_phys, aligned_span;

    base = 0;
    ready = 0;
    pixels_written = 0;

    if (!(mbi->flags & MULTIBOOT_INFO_FRAMEBUFFER)) return FB64_NO_MODE;
    if (mbi->framebuffer_type != MULTIBOOT_FRAMEBUFFER_TYPE_RGB)
        return FB64_NOT_RGB;
    /* 24bpp is packed three bytes per pixel and 32bpp four. Anything else
     * - 16bpp 5:6:5, 8bpp indexed - would need a different packing than
     * fb64_rgb does, so refuse rather than draw a plausible mess. */
    if (mbi->framebuffer_bpp != 32 && mbi->framebuffer_bpp != 24)
        return FB64_BAD_DEPTH;
    if (!mbi->framebuffer_addr || !mbi->framebuffer_width ||
        !mbi->framebuffer_height || !mbi->framebuffer_pitch)
        return FB64_NO_MODE;

    phys_addr = mbi->framebuffer_addr;
    pitch     = mbi->framebuffer_pitch;
    width     = mbi->framebuffer_width;
    height    = mbi->framebuffer_height;
    depth     = mbi->framebuffer_bpp;
    bytes_per_pixel = depth / 8;

    span = (uint64_t)pitch * height;

    /* 2MB pages need a 2MB-aligned start, and the mapping has to cover
     * the whole framebuffer even when its base is not aligned - so round
     * the base down and the length up, and remember the offset. */
    aligned_phys = phys_addr & ~HUGE_MASK;
    aligned_span = (span + (phys_addr - aligned_phys) + HUGE_MASK)
                   & ~HUGE_MASK;

    if (paging64_map_mmio(MMIO64_BASE, aligned_phys, aligned_span)
            != PAGING64_OK)
        return FB64_NO_MAP;

    base = (uint8_t*)(MMIO64_BASE + (phys_addr - aligned_phys));
    mapped_bytes = span;
    ready = 1;
    return FB64_OK;
}

/* Publishes the framebuffer as /dev/fb0, once the filesystem exists.
 * Separate from fb64_init because the driver comes up in layer 6c and
 * the filesystem several layers later - and because a device that
 * nothing can open is still a working driver, just an unreachable one. */
int fb64_register(void) {
    int node;

    if (!ready) return 0;

    if (ramfs64_create("/dev", 1) < 0 && ramfs64_lookup("/dev") < 0)
        return 0;

    node = ramfs64_create("/dev/fb0", 0);
    if (node < 0) node = ramfs64_lookup("/dev/fb0");
    if (node < 0) return 0;

    return ramfs64_set_device(node, RAMFS64_DEV_FB);
}

int      fb64_ready(void)  { return ready; }
uint32_t fb64_width(void)  { return width; }
uint32_t fb64_height(void) { return height; }
uint32_t fb64_pitch(void)  { return pitch; }
uint32_t fb64_bpp(void)    { return depth; }
uint64_t fb64_phys(void)   { return phys_addr; }
uint64_t fb64_bytes(void)  { return mapped_bytes; }
uint64_t fb64_pixels_written(void) { return pixels_written; }

uint32_t fb64_rgb(uint8_t r, uint8_t g, uint8_t b) {
    /* Both depths this driver accepts are packed B,G,R[,x] in memory
     * order on little-endian x86, so one 0x00RRGGBB value describes a
     * pixel in either - the fourth byte is just padding at 24bpp. */
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

void fb64_put_pixel(uint32_t x, uint32_t y, uint32_t color) {
    uint8_t* p;
    if (!ready || x >= width || y >= height) return;
    p = base + (uint64_t)y * pitch + (uint64_t)x * bytes_per_pixel;
    p[0] = (uint8_t)(color);
    p[1] = (uint8_t)(color >> 8);
    p[2] = (uint8_t)(color >> 16);
    if (bytes_per_pixel == 4) p[3] = 0;
    pixels_written++;
}

uint32_t fb64_get_pixel(uint32_t x, uint32_t y) {
    const uint8_t* p;
    if (!ready || x >= width || y >= height) return 0;
    p = base + (uint64_t)y * pitch + (uint64_t)x * bytes_per_pixel;
    return fb64_rgb(p[2], p[1], p[0]);
}

void fb64_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                    uint32_t color) {
    uint32_t xx, yy, x_end, y_end;
    if (!ready) return;

    x_end = x + w;
    y_end = y + h;
    if (x_end > width)  x_end = width;
    if (y_end > height) y_end = height;

    for (yy = y; yy < y_end; yy++) {
        if (bytes_per_pixel == 4) {
            /* A row of 32bpp pixels is a row of uint32_t, so the inner
             * loop is one store per pixel rather than four. */
            uint32_t* row = (uint32_t*)(base + (uint64_t)yy * pitch);
            for (xx = x; xx < x_end; xx++) row[xx] = color & 0x00FFFFFFu;
            pixels_written += x_end - x;
        } else {
            for (xx = x; xx < x_end; xx++) fb64_put_pixel(xx, yy, color);
        }
    }
}

void fb64_clear(uint32_t color) {
    fb64_fill_rect(0, 0, width, height, color);
}

void fb64_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
               const uint32_t* src, uint32_t src_pitch) {
    uint32_t row, i;
    if (!ready || x >= width || y >= height) return;
    if (x + w > width)  w = width - x;
    if (y + h > height) h = height - y;

    for (row = 0; row < h; row++) {
        const uint32_t* s = src + (uint64_t)row * src_pitch;
        uint8_t* d = base + (uint64_t)(y + row) * pitch
                          + (uint64_t)x * bytes_per_pixel;
        if (bytes_per_pixel == 4) {
            uint32_t* d32 = (uint32_t*)d;
            for (i = 0; i < w; i++) d32[i] = s[i] & 0x00FFFFFFu;
        } else {
            for (i = 0; i < w; i++) {
                uint32_t c = s[i];
                d[0] = (uint8_t)(c);
                d[1] = (uint8_t)(c >> 8);
                d[2] = (uint8_t)(c >> 16);
                d += 3;
            }
        }
        pixels_written += w;
    }
}
