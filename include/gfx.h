#ifndef GFX_H
#define GFX_H

#include <stdint.h>
#include "uifont.h"

/* gfx - the software rendering layer the desktop is composited with.
 *
 * Everything above this file (the window manager, the Dock, the apps)
 * draws into an off-screen 32-bit surface and never touches the hardware
 * framebuffer directly. That indirection buys three things a
 * draw-straight-to-VRAM design can't have:
 *
 *   - no tearing or flicker: a frame is assembled completely, then
 *     pushed to the framebuffer in one pass (gfx_present);
 *   - alpha: translucency, soft shadows and antialiased edges all need
 *     to read the pixel underneath, and reading back from VRAM over the
 *     PCI bus is famously slow;
 *   - windows can own their own surface, so moving one is a blit rather
 *     than asking its app to redraw.
 *
 * Colors are 0xAARRGGBB. The alpha byte is honored by every drawing
 * call; 0xFF is opaque, and a color written with GFX_RGB() is opaque.
 * Surfaces themselves are treated as opaque - compositing always ends up
 * on a fully-covered background - so no alpha is stored per pixel. */

#define GFX_ARGB(a, r, g, b) (((uint32_t)(a) << 24) | ((uint32_t)(r) << 16) | \
                              ((uint32_t)(g) << 8) | (uint32_t)(b))
#define GFX_RGB(r, g, b)     GFX_ARGB(0xFF, (r), (g), (b))
#define GFX_ALPHA(c)         (((c) >> 24) & 0xFF)
#define GFX_RED(c)           (((c) >> 16) & 0xFF)
#define GFX_GREEN(c)         (((c) >> 8) & 0xFF)
#define GFX_BLUE(c)          ((c) & 0xFF)

/* Same color, different opacity. */
#define GFX_WITH_ALPHA(c, a) (((c) & 0x00FFFFFFu) | ((uint32_t)(a) << 24))

typedef struct {
    uint32_t* pixels;
    int w, h;       /* logical size: what gets drawn and blitted */
    int pitch;      /* pixels per row of the allocation (>= w) */
    int cap_h;      /* rows in the allocation (>= h) */
} gfx_surface_t;

typedef struct {
    int x, y, w, h;
} gfx_rect_t;

/* --- surfaces ---------------------------------------------------------- */

/* kmalloc's a w*h surface. Returns 0 if the heap can't satisfy it, which
 * callers are expected to handle (a window that can't get a backing store
 * simply doesn't open). */
gfx_surface_t* gfx_surface_create(int w, int h);
void gfx_surface_destroy(gfx_surface_t* s);

/* Changes a surface's logical size, reusing the existing allocation when
 * it's already big enough. Dragging a window's resize corner asks for a
 * new size on every mouse packet, and round-tripping a megabyte through
 * kmalloc/kfree at that rate would shred the heap - so allocations are
 * rounded up and shrinking is free. Returns 0 if a needed grow failed, in
 * which case the surface is untouched. */
int gfx_surface_resize(gfx_surface_t* s, int w, int h);

/* Directs subsequent drawing at a surface, resetting the origin to (0,0)
 * and the clip to the whole surface. */
void gfx_set_target(gfx_surface_t* s);
gfx_surface_t* gfx_get_target(void);

/* Drawing state: an origin (added to every coordinate passed in) and a
 * clip rectangle. Both are saved and restored as one unit, which is how
 * the window manager hands an app a client area to draw in without the
 * app knowing where on screen it is. */
void gfx_save(void);
void gfx_restore(void);
void gfx_origin(int dx, int dy);            /* shifts the current origin */
int  gfx_clip(int x, int y, int w, int h);  /* intersects; 0 if now empty */
void gfx_clip_reset(void);
gfx_rect_t gfx_clip_rect(void);             /* current clip, surface coords */

/* --- primitives -------------------------------------------------------- */

void gfx_clear(uint32_t color);
void gfx_fill(int x, int y, int w, int h, uint32_t color);
void gfx_pixel(int x, int y, uint32_t color);
void gfx_hline(int x, int y, int w, uint32_t color);
void gfx_vline(int x, int y, int h, uint32_t color);
void gfx_frame(int x, int y, int w, int h, uint32_t color);      /* 1px outline */
void gfx_vgradient(int x, int y, int w, int h, uint32_t top, uint32_t bottom);

/* Antialiased rounded rectangles - the shape everything in this UI is
 * made of. `radius` is clamped to half the smaller side. */
void gfx_round_rect(int x, int y, int w, int h, int radius, uint32_t color);
void gfx_round_rect_gradient(int x, int y, int w, int h, int radius,
                             uint32_t top, uint32_t bottom);
void gfx_round_frame(int x, int y, int w, int h, int radius, int thickness,
                     uint32_t color);

void gfx_circle(int cx, int cy, int radius, uint32_t color);
/* A four-pointed star (an astroid: sqrt|x| + sqrt|y| <= sqrt|r|). This is
 * the Novaris mark - it appears at the left of the menu bar, on the Dock
 * icon and in the About window. */
void gfx_sparkle(int cx, int cy, int radius, uint32_t color);
void gfx_ring(int cx, int cy, int radius, int thickness, uint32_t color);

/* A soft drop shadow for a rounded rect: alpha falls off over `blur`
 * pixels outside the shape's edge. The shape's own area is left alone,
 * so this is drawn first and the window blitted over it. */
void gfx_shadow(int x, int y, int w, int h, int radius, int blur, int alpha);

/* Box-blurs a region of the target in place (two passes, which is close
 * enough to a gaussian at these radii). This is what makes the menu bar
 * and the Dock read as translucent panels over the wallpaper rather than
 * as flat washes of color. */
void gfx_blur(int x, int y, int w, int h, int radius);

/* --- blits ------------------------------------------------------------- */

void gfx_blit(const gfx_surface_t* src, int x, int y);
void gfx_blit_alpha(const gfx_surface_t* src, int x, int y, int alpha);
/* Blit with the source's corners rounded off (antialiased) - how a window
 * with rounded corners gets composited over the desktop. */
void gfx_blit_rounded(const gfx_surface_t* src, int x, int y, int radius);
/* Nearest-neighbour scale into a w*h box. Used for the live thumbnails
 * minimized windows show in the Dock. */
void gfx_blit_scaled(const gfx_surface_t* src, int x, int y, int w, int h);
/* Copies a rectangle straight across between same-sized surfaces, ignoring
 * origin/clip. Used to stamp the pre-rendered wallpaper into the frame. */
void gfx_copy_region(gfx_surface_t* dst, const gfx_surface_t* src,
                     int x, int y, int w, int h);

/* --- text -------------------------------------------------------------- */

/* Draws `s` with its line box's top-left at (x, y); returns the pen's
 * final x. The font's own ascent positions the baseline. */
int  gfx_text(const uifont_t* font, int x, int y, const char* s, uint32_t color);
int  gfx_text_n(const uifont_t* font, int x, int y, const char* s, int n,
                uint32_t color);
int  gfx_text_width(const uifont_t* font, const char* s);
int  gfx_text_width_n(const uifont_t* font, const char* s, int n);
/* Centers within [x, x+w) and right-aligns at x+w respectively. */
void gfx_text_center(const uifont_t* font, int x, int y, int w, const char* s,
                     uint32_t color);
void gfx_text_right(const uifont_t* font, int x, int y, int w, const char* s,
                    uint32_t color);
/* Truncates with an ellipsis if it doesn't fit in `max_w`. */
void gfx_text_ellipsized(const uifont_t* font, int x, int y, int max_w,
                         const char* s, uint32_t color);

/* The 8x16 terminal font, for the fixed character grid. */
#define GFX_MONO_W 8
#define GFX_MONO_H 16
void gfx_mono_char(int x, int y, char c, uint32_t color);
void gfx_mono_text(int x, int y, const char* s, uint32_t color);

/* --- presenting -------------------------------------------------------- */

/* Pushes a rectangle of the target surface to the hardware framebuffer. */
void gfx_present(const gfx_surface_t* s, int x, int y, int w, int h);

/* --- rect helpers ------------------------------------------------------ */

int  gfx_rect_contains(const gfx_rect_t* r, int x, int y);
void gfx_rect_union(gfx_rect_t* into, const gfx_rect_t* other);
int  gfx_rect_intersects(const gfx_rect_t* a, const gfx_rect_t* b);

#endif
