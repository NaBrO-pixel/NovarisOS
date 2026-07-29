/* gfx.c - software rendering onto off-screen 32-bit surfaces.
 *
 * See include/gfx.h for why the desktop composites instead of drawing
 * straight into VRAM. The interesting parts of this file are:
 *
 *   - rr_distance(): a signed distance function for a rounded rectangle.
 *     Every rounded corner, every antialiased edge and every drop shadow
 *     in the UI comes out of this one function - "how far is this pixel
 *     from the shape's boundary?" answers both "how much of this pixel is
 *     covered?" (an edge) and "how dark is the shadow here?" at once.
 *     It's fixed-point (1/256 px) because the kernel has no FPU state
 *     saved across interrupts and so avoids floating point entirely.
 *   - the glyph blender, which turns the 8-bit coverage maps in
 *     kernel/uifont.c into antialiased text.
 *   - gfx_blur(), a two-pass sliding-window box blur that gives the menu
 *     bar and the Dock their frosted look.
 */

#include "gfx.h"
#include "framebuffer.h"
#include "font8x16.h"
#include "kheap.h"
#include "kstring.h"

#define STATE_STACK_DEPTH 12
#define FP 256   /* fixed-point scale: one pixel */

typedef struct {
    int ox, oy;        /* origin added to incoming coordinates */
    gfx_rect_t clip;   /* in surface coordinates */
} gfx_state_t;

static gfx_surface_t* target;
static gfx_state_t state;
static gfx_state_t state_stack[STATE_STACK_DEPTH];
static int state_sp;

/* --- small math -------------------------------------------------------- */

static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }
static inline int iclamp(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Integer square root, bit-by-bit. Only ever called on the handful of
 * pixels near a corner, so its cost doesn't matter much - correctness
 * without floating point does. */
static uint32_t isqrt32(uint32_t value) {
    uint32_t result = 0;
    uint32_t bit = 1u << 30;
    while (bit > value) bit >>= 2;
    while (bit != 0) {
        if (value >= result + bit) {
            value -= result + bit;
            result = (result >> 1) + bit;
        } else {
            result >>= 1;
        }
        bit >>= 2;
    }
    return result;
}

/* Source over destination. `a` is 0-255 coverage/opacity. Destination is
 * always opaque, so the result is too. */
static inline uint32_t blend(uint32_t dst, uint32_t src, uint32_t a) {
    if (a == 0) return dst;
    if (a >= 255) return 0xFF000000u | src;

    uint32_t ia = 255 - a;
    uint32_t r = ((src >> 16) & 0xFF) * a + ((dst >> 16) & 0xFF) * ia + 128;
    uint32_t g = ((src >> 8) & 0xFF) * a + ((dst >> 8) & 0xFF) * ia + 128;
    uint32_t b = (src & 0xFF) * a + (dst & 0xFF) * ia + 128;
    r = (r + (r >> 8)) >> 8;
    g = (g + (g >> 8)) >> 8;
    b = (b + (b >> 8)) >> 8;
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

/* --- surfaces ---------------------------------------------------------- */

gfx_surface_t* gfx_surface_create(int w, int h) {
    if (w <= 0 || h <= 0) return 0;
    gfx_surface_t* s = (gfx_surface_t*)kmalloc(sizeof(gfx_surface_t));
    if (!s) return 0;
    s->pixels = (uint32_t*)kmalloc((uint32_t)w * (uint32_t)h * 4u);
    if (!s->pixels) {
        kfree(s);
        return 0;
    }
    s->w = w;
    s->h = h;
    s->pitch = w;
    s->cap_h = h;
    return s;
}

int gfx_surface_resize(gfx_surface_t* s, int w, int h) {
    if (!s || w <= 0 || h <= 0) return 0;
    if (w <= s->pitch && h <= s->cap_h) {
        s->w = w;
        s->h = h;
        return 1;
    }
    /* Round the new allocation up so a drag that grows a window pixel by
     * pixel doesn't reallocate on every packet. */
    int pitch = (w > s->pitch ? (w + 63) & ~63 : s->pitch);
    int rows = (h > s->cap_h ? (h + 63) & ~63 : s->cap_h);
    uint32_t* pixels = (uint32_t*)kmalloc((uint32_t)pitch * (uint32_t)rows * 4u);
    if (!pixels) return 0;

    kfree(s->pixels);
    s->pixels = pixels;
    s->pitch = pitch;
    s->cap_h = rows;
    s->w = w;
    s->h = h;
    return 1;
}

void gfx_surface_destroy(gfx_surface_t* s) {
    if (!s) return;
    kfree(s->pixels);
    kfree(s);
}

void gfx_set_target(gfx_surface_t* s) {
    target = s;
    state.ox = 0;
    state.oy = 0;
    state.clip.x = 0;
    state.clip.y = 0;
    state.clip.w = s ? s->w : 0;
    state.clip.h = s ? s->h : 0;
    state_sp = 0;
}

gfx_surface_t* gfx_get_target(void) { return target; }

void gfx_save(void) {
    if (state_sp < STATE_STACK_DEPTH) state_stack[state_sp] = state;
    state_sp++;
}

void gfx_restore(void) {
    if (state_sp > 0) {
        state_sp--;
        if (state_sp < STATE_STACK_DEPTH) state = state_stack[state_sp];
    }
}

void gfx_origin(int dx, int dy) {
    state.ox += dx;
    state.oy += dy;
}

int gfx_clip(int x, int y, int w, int h) {
    int x0 = imax(state.clip.x, x + state.ox);
    int y0 = imax(state.clip.y, y + state.oy);
    int x1 = imin(state.clip.x + state.clip.w, x + state.ox + w);
    int y1 = imin(state.clip.y + state.clip.h, y + state.oy + h);
    state.clip.x = x0;
    state.clip.y = y0;
    state.clip.w = x1 > x0 ? x1 - x0 : 0;
    state.clip.h = y1 > y0 ? y1 - y0 : 0;
    return state.clip.w > 0 && state.clip.h > 0;
}

void gfx_clip_reset(void) {
    state.clip.x = 0;
    state.clip.y = 0;
    state.clip.w = target ? target->w : 0;
    state.clip.h = target ? target->h : 0;
}

gfx_rect_t gfx_clip_rect(void) { return state.clip; }

/* --- primitives -------------------------------------------------------- */

/* Every drawing call funnels through here: absolute coordinates, already
 * clipped by the caller. */
static inline void put(int x, int y, uint32_t color, uint32_t a) {
    uint32_t* p = &target->pixels[y * target->pitch + x];
    *p = blend(*p, color, a);
}

void gfx_pixel(int x, int y, uint32_t color) {
    if (!target) return;
    x += state.ox;
    y += state.oy;
    if (x < state.clip.x || y < state.clip.y ||
        x >= state.clip.x + state.clip.w || y >= state.clip.y + state.clip.h) {
        return;
    }
    put(x, y, color, GFX_ALPHA(color));
}

void gfx_fill(int x, int y, int w, int h, uint32_t color) {
    if (!target || w <= 0 || h <= 0) return;
    int x0 = imax(x + state.ox, state.clip.x);
    int y0 = imax(y + state.oy, state.clip.y);
    int x1 = imin(x + state.ox + w, state.clip.x + state.clip.w);
    int y1 = imin(y + state.oy + h, state.clip.y + state.clip.h);
    if (x0 >= x1 || y0 >= y1) return;

    uint32_t a = GFX_ALPHA(color);
    if (a == 0) return;

    if (a == 255) {
        uint32_t opaque = 0xFF000000u | color;
        for (int yy = y0; yy < y1; yy++) {
            uint32_t* row = &target->pixels[yy * target->pitch + x0];
            for (int xx = x0; xx < x1; xx++) *row++ = opaque;
        }
        return;
    }
    for (int yy = y0; yy < y1; yy++) {
        uint32_t* row = &target->pixels[yy * target->pitch + x0];
        for (int xx = x0; xx < x1; xx++) {
            *row = blend(*row, color, a);
            row++;
        }
    }
}

void gfx_clear(uint32_t color) {
    gfx_fill(-state.ox, -state.oy, target ? target->w : 0,
             target ? target->h : 0, color);
}

void gfx_hline(int x, int y, int w, uint32_t color) { gfx_fill(x, y, w, 1, color); }
void gfx_vline(int x, int y, int h, uint32_t color) { gfx_fill(x, y, 1, h, color); }

void gfx_frame(int x, int y, int w, int h, uint32_t color) {
    if (w <= 0 || h <= 0) return;
    gfx_fill(x, y, w, 1, color);
    gfx_fill(x, y + h - 1, w, 1, color);
    gfx_fill(x, y + 1, 1, h - 2, color);
    gfx_fill(x + w - 1, y + 1, 1, h - 2, color);
}

/* Linear interpolation between two colors, t in 0-255. */
static uint32_t lerp_color(uint32_t a, uint32_t b, uint32_t t) {
    uint32_t it = 255 - t;
    uint32_t al = (GFX_ALPHA(a) * it + GFX_ALPHA(b) * t) / 255;
    uint32_t r = (GFX_RED(a) * it + GFX_RED(b) * t) / 255;
    uint32_t g = (GFX_GREEN(a) * it + GFX_GREEN(b) * t) / 255;
    uint32_t bl = (GFX_BLUE(a) * it + GFX_BLUE(b) * t) / 255;
    return GFX_ARGB(al, r, g, bl);
}

void gfx_vgradient(int x, int y, int w, int h, uint32_t top, uint32_t bottom) {
    if (h <= 0) return;
    for (int i = 0; i < h; i++) {
        uint32_t t = h > 1 ? (uint32_t)(i * 255) / (uint32_t)(h - 1) : 0;
        gfx_fill(x, y + i, w, 1, lerp_color(top, bottom, t));
    }
}

/* Signed distance (in 1/256 px) from a pixel center to the boundary of a
 * rounded rectangle: negative inside, positive outside. This is the whole
 * geometry engine - see the file header. */
static inline int32_t rr_distance(int32_t px, int32_t py, int32_t cx, int32_t cy,
                                  int32_t half_w, int32_t half_h, int32_t r) {
    int32_t qx = (px > cx ? px - cx : cx - px) - (half_w - r);
    int32_t qy = (py > cy ? py - cy : cy - py) - (half_h - r);
    if (qx <= 0 && qy <= 0) {
        /* Inside the straight part: the nearer of the two edges wins. */
        return (qx > qy ? qx : qy) - r;
    }
    int32_t mx = qx > 0 ? qx : 0;
    int32_t my = qy > 0 ? qy : 0;
    /* Guard the multiply: anything this far out is nowhere near the edge,
     * and squaring it would overflow. */
    if (mx > 40 * FP || my > 40 * FP) return (mx > my ? mx : my) - r;
    return (int32_t)isqrt32((uint32_t)(mx * mx + my * my)) - r;
}

/* Shared inner loop for a rounded rect: calls back per row with the span
 * to fill solid, and blends the antialiased corner pixels itself. */
static void round_rect_rows(int x, int y, int w, int h, int radius,
                            uint32_t color_top, uint32_t color_bottom,
                            int gradient) {
    if (!target || w <= 0 || h <= 0) return;
    if (radius * 2 > w) radius = w / 2;
    if (radius * 2 > h) radius = h / 2;
    if (radius < 0) radius = 0;

    int ax = x + state.ox, ay = y + state.oy;
    int x0 = imax(ax, state.clip.x);
    int y0 = imax(ay, state.clip.y);
    int x1 = imin(ax + w, state.clip.x + state.clip.w);
    int y1 = imin(ay + h, state.clip.y + state.clip.h);
    if (x0 >= x1 || y0 >= y1) return;

    int32_t cx = ax * FP + w * FP / 2;
    int32_t cy = ay * FP + h * FP / 2;
    int32_t hw = w * FP / 2, hh = h * FP / 2, r = radius * FP;
    uint32_t base_alpha = GFX_ALPHA(color_top);

    for (int py = y0; py < y1; py++) {
        uint32_t color = color_top;
        if (gradient) {
            uint32_t t = h > 1 ? (uint32_t)((py - ay) * 255) / (uint32_t)(h - 1) : 0;
            color = lerp_color(color_top, color_bottom, t);
            base_alpha = GFX_ALPHA(color);
        }

        /* Rows through the straight middle of the shape have no curvature
         * to resolve, so they're a plain span fill. */
        int in_corner_band = (py < ay + radius) || (py >= ay + h - radius);
        if (!in_corner_band) {
            uint32_t* row = &target->pixels[py * target->pitch + x0];
            for (int px = x0; px < x1; px++) {
                *row = blend(*row, color, base_alpha);
                row++;
            }
            continue;
        }

        int32_t fy = py * FP + FP / 2;
        uint32_t* row = &target->pixels[py * target->pitch + x0];
        for (int px = x0; px < x1; px++) {
            /* Only the corner columns can be partially covered. */
            if (px >= ax + radius && px < ax + w - radius) {
                *row = blend(*row, color, base_alpha);
            } else {
                int32_t d = rr_distance(px * FP + FP / 2, fy, cx, cy, hw, hh, r);
                int cov = iclamp(FP / 2 - d, 0, 255);
                if (cov) *row = blend(*row, color, (uint32_t)cov * base_alpha / 255);
            }
            row++;
        }
    }
}

void gfx_round_rect(int x, int y, int w, int h, int radius, uint32_t color) {
    round_rect_rows(x, y, w, h, radius, color, color, 0);
}

void gfx_round_rect_gradient(int x, int y, int w, int h, int radius,
                             uint32_t top, uint32_t bottom) {
    round_rect_rows(x, y, w, h, radius, top, bottom, 1);
}

void gfx_round_frame(int x, int y, int w, int h, int radius, int thickness,
                     uint32_t color) {
    if (!target || w <= 0 || h <= 0 || thickness <= 0) return;
    if (radius * 2 > w) radius = w / 2;
    if (radius * 2 > h) radius = h / 2;

    int ax = x + state.ox, ay = y + state.oy;
    int x0 = imax(ax, state.clip.x);
    int y0 = imax(ay, state.clip.y);
    int x1 = imin(ax + w, state.clip.x + state.clip.w);
    int y1 = imin(ay + h, state.clip.y + state.clip.h);
    if (x0 >= x1 || y0 >= y1) return;

    int32_t cx = ax * FP + w * FP / 2;
    int32_t cy = ay * FP + h * FP / 2;
    int32_t hw = w * FP / 2, hh = h * FP / 2, r = radius * FP;
    uint32_t alpha = GFX_ALPHA(color);
    int32_t t = thickness * FP;

    for (int py = y0; py < y1; py++) {
        int32_t fy = py * FP + FP / 2;
        uint32_t* row = &target->pixels[py * target->pitch + x0];
        for (int px = x0; px < x1; px++) {
            int32_t d = rr_distance(px * FP + FP / 2, fy, cx, cy, hw, hh, r);
            /* Distance to the *stroke*: a band of width `thickness` just
             * inside the boundary. */
            int32_t band = (d + t > 0 ? d + t : -(d + t)) - t;
            int cov = iclamp(FP / 2 - band, 0, 255);
            if (cov) *row = blend(*row, color, (uint32_t)cov * alpha / 255);
            row++;
        }
    }
}

void gfx_circle(int cx, int cy, int radius, uint32_t color) {
    gfx_round_rect(cx - radius, cy - radius, radius * 2, radius * 2, radius, color);
}

void gfx_sparkle(int cx, int cy, int radius, uint32_t color) {
    if (!target || radius <= 0) return;
    int ax = cx + state.ox, ay = cy + state.oy;
    int x0 = imax(ax - radius, state.clip.x), y0 = imax(ay - radius, state.clip.y);
    int x1 = imin(ax + radius + 1, state.clip.x + state.clip.w);
    int y1 = imin(ay + radius + 1, state.clip.y + state.clip.h);
    if (x0 >= x1 || y0 >= y1) return;

    /* The astroid's edge is a curve no distance function of ours covers,
     * so coverage comes from 3x3 supersampling instead - a 9-level ramp,
     * which at this size is indistinguishable from a smooth one. */
    uint32_t limit = isqrt32((uint32_t)radius * FP);
    uint32_t alpha = GFX_ALPHA(color);

    for (int py = y0; py < y1; py++) {
        uint32_t* d = &target->pixels[py * target->pitch + x0];
        for (int px = x0; px < x1; px++) {
            int inside = 0;
            for (int sy = 0; sy < 3; sy++) {
                for (int sx = 0; sx < 3; sx++) {
                    int dx = (px - ax) * 3 + sx - 1;
                    int dy = (py - ay) * 3 + sy - 1;
                    uint32_t adx = (uint32_t)(dx < 0 ? -dx : dx) * FP / 3;
                    uint32_t ady = (uint32_t)(dy < 0 ? -dy : dy) * FP / 3;
                    if (isqrt32(adx) + isqrt32(ady) <= limit) inside++;
                }
            }
            if (inside) *d = blend(*d, color, (uint32_t)inside * alpha / 9);
            d++;
        }
    }
}

void gfx_ring(int cx, int cy, int radius, int thickness, uint32_t color) {
    gfx_round_frame(cx - radius, cy - radius, radius * 2, radius * 2, radius,
                    thickness, color);
}

void gfx_shadow(int x, int y, int w, int h, int radius, int blur, int alpha) {
    if (!target || w <= 0 || h <= 0 || blur <= 0) return;
    if (radius * 2 > w) radius = w / 2;
    if (radius * 2 > h) radius = h / 2;

    int ax = x + state.ox, ay = y + state.oy;
    int x0 = imax(ax - blur, state.clip.x);
    int y0 = imax(ay - blur, state.clip.y);
    int x1 = imin(ax + w + blur, state.clip.x + state.clip.w);
    int y1 = imin(ay + h + blur, state.clip.y + state.clip.h);
    if (x0 >= x1 || y0 >= y1) return;

    int32_t cx = ax * FP + w * FP / 2;
    int32_t cy = ay * FP + h * FP / 2;
    int32_t hw = w * FP / 2, hh = h * FP / 2, r = radius * FP;
    int32_t blur_fp = blur * FP;

    for (int py = y0; py < y1; py++) {
        int32_t fy = py * FP + FP / 2;
        uint32_t* row = &target->pixels[py * target->pitch + x0];
        /* Rows level with the shape's straight middle only shade near the
         * left and right edges - skip the interior columns wholesale. */
        for (int px = x0; px < x1; px++) {
            if (px >= ax && px < ax + w && py >= ay && py < ay + h) {
                /* Under the window itself: it will be drawn over this, so
                 * shading here is wasted work. */
                int skip_to = imin(ax + w, x1);
                row += skip_to - px;
                px = skip_to - 1;
                continue;
            }
            int32_t d = rr_distance(px * FP + FP / 2, fy, cx, cy, hw, hh, r);
            if (d <= 0) { row++; continue; }
            if (d >= blur_fp) { row++; continue; }
            /* Quadratic falloff: soft, and cheap without a real gaussian. */
            int32_t t = blur_fp - d;                    /* 0..blur_fp */
            int32_t f = (t * 255) / blur_fp;            /* 0..255 */
            uint32_t a = (uint32_t)((f * f) / 255 * alpha / 255);
            if (a) *row = blend(*row, 0x000000u, a);
            row++;
        }
    }
}

/* --- blur -------------------------------------------------------------- */

#define BLUR_MAX_SPAN 2048
static uint32_t blur_line[BLUR_MAX_SPAN];

static void blur_pass_h(int x, int y, int w, int h, int radius) {
    if (w > BLUR_MAX_SPAN) return;
    int window = radius * 2 + 1;
    for (int row = 0; row < h; row++) {
        uint32_t* p = &target->pixels[(y + row) * target->pitch + x];
        for (int i = 0; i < w; i++) blur_line[i] = p[i];

        uint32_t sr = 0, sg = 0, sb = 0;
        for (int i = -radius; i <= radius; i++) {
            uint32_t c = blur_line[iclamp(i, 0, w - 1)];
            sr += (c >> 16) & 0xFF; sg += (c >> 8) & 0xFF; sb += c & 0xFF;
        }
        for (int i = 0; i < w; i++) {
            p[i] = 0xFF000000u | ((sr / window) << 16) | ((sg / window) << 8) |
                   (sb / window);
            uint32_t out = blur_line[iclamp(i - radius, 0, w - 1)];
            uint32_t in = blur_line[iclamp(i + radius + 1, 0, w - 1)];
            sr += ((in >> 16) & 0xFF) - ((out >> 16) & 0xFF);
            sg += ((in >> 8) & 0xFF) - ((out >> 8) & 0xFF);
            sb += (in & 0xFF) - (out & 0xFF);
        }
    }
}

static void blur_pass_v(int x, int y, int w, int h, int radius) {
    if (h > BLUR_MAX_SPAN) return;
    int window = radius * 2 + 1;
    int pitch = target->pitch;
    for (int col = 0; col < w; col++) {
        uint32_t* p = &target->pixels[y * pitch + x + col];
        for (int i = 0; i < h; i++) blur_line[i] = p[i * pitch];

        uint32_t sr = 0, sg = 0, sb = 0;
        for (int i = -radius; i <= radius; i++) {
            uint32_t c = blur_line[iclamp(i, 0, h - 1)];
            sr += (c >> 16) & 0xFF; sg += (c >> 8) & 0xFF; sb += c & 0xFF;
        }
        for (int i = 0; i < h; i++) {
            p[i * pitch] = 0xFF000000u | ((sr / window) << 16) |
                           ((sg / window) << 8) | (sb / window);
            uint32_t out = blur_line[iclamp(i - radius, 0, h - 1)];
            uint32_t in = blur_line[iclamp(i + radius + 1, 0, h - 1)];
            sr += ((in >> 16) & 0xFF) - ((out >> 16) & 0xFF);
            sg += ((in >> 8) & 0xFF) - ((out >> 8) & 0xFF);
            sb += (in & 0xFF) - (out & 0xFF);
        }
    }
}

void gfx_blur(int x, int y, int w, int h, int radius) {
    if (!target || radius < 1) return;
    /* Deliberately ignores the clip: a blur reads its neighbourhood, so
     * blurring only the clipped part would make the result depend on the
     * damage rectangle. Callers (the desktop compositor) instead grow the
     * damage region to cover any panel they're about to blur. */
    int x0 = imax(x + state.ox, 0);
    int y0 = imax(y + state.oy, 0);
    int x1 = imin(x + state.ox + w, target->w);
    int y1 = imin(y + state.oy + h, target->h);
    if (x0 >= x1 || y0 >= y1) return;

    /* Two box passes approximate a gaussian well enough at this radius. */
    for (int pass = 0; pass < 2; pass++) {
        blur_pass_h(x0, y0, x1 - x0, y1 - y0, radius);
        blur_pass_v(x0, y0, x1 - x0, y1 - y0, radius);
    }
}

/* --- blits ------------------------------------------------------------- */

void gfx_blit(const gfx_surface_t* src, int x, int y) {
    if (!target || !src) return;
    int ax = x + state.ox, ay = y + state.oy;
    int x0 = imax(ax, state.clip.x), y0 = imax(ay, state.clip.y);
    int x1 = imin(ax + src->w, state.clip.x + state.clip.w);
    int y1 = imin(ay + src->h, state.clip.y + state.clip.h);
    if (x0 >= x1 || y0 >= y1) return;

    for (int py = y0; py < y1; py++) {
        const uint32_t* s = &src->pixels[(py - ay) * src->pitch + (x0 - ax)];
        uint32_t* d = &target->pixels[py * target->pitch + x0];
        kmemcpy(d, s, (uint32_t)(x1 - x0) * 4u);
    }
}

void gfx_blit_alpha(const gfx_surface_t* src, int x, int y, int alpha) {
    if (!target || !src) return;
    if (alpha >= 255) { gfx_blit(src, x, y); return; }
    if (alpha <= 0) return;
    int ax = x + state.ox, ay = y + state.oy;
    int x0 = imax(ax, state.clip.x), y0 = imax(ay, state.clip.y);
    int x1 = imin(ax + src->w, state.clip.x + state.clip.w);
    int y1 = imin(ay + src->h, state.clip.y + state.clip.h);
    if (x0 >= x1 || y0 >= y1) return;

    for (int py = y0; py < y1; py++) {
        const uint32_t* s = &src->pixels[(py - ay) * src->pitch + (x0 - ax)];
        uint32_t* d = &target->pixels[py * target->pitch + x0];
        for (int px = x0; px < x1; px++) {
            *d = blend(*d, *s++, (uint32_t)alpha);
            d++;
        }
    }
}

void gfx_blit_rounded(const gfx_surface_t* src, int x, int y, int radius) {
    if (!target || !src) return;
    int w = src->w, h = src->h;
    if (radius * 2 > w) radius = w / 2;
    if (radius * 2 > h) radius = h / 2;

    int ax = x + state.ox, ay = y + state.oy;
    int x0 = imax(ax, state.clip.x), y0 = imax(ay, state.clip.y);
    int x1 = imin(ax + w, state.clip.x + state.clip.w);
    int y1 = imin(ay + h, state.clip.y + state.clip.h);
    if (x0 >= x1 || y0 >= y1) return;

    int32_t cx = ax * FP + w * FP / 2, cy = ay * FP + h * FP / 2;
    int32_t hw = w * FP / 2, hh = h * FP / 2, r = radius * FP;

    for (int py = y0; py < y1; py++) {
        const uint32_t* s = &src->pixels[(py - ay) * src->pitch + (x0 - ax)];
        uint32_t* d = &target->pixels[py * target->pitch + x0];
        int in_corner_band = (py < ay + radius) || (py >= ay + h - radius);
        if (!in_corner_band) {
            kmemcpy(d, s, (uint32_t)(x1 - x0) * 4u);
            continue;
        }
        int32_t fy = py * FP + FP / 2;
        for (int px = x0; px < x1; px++) {
            if (px >= ax + radius && px < ax + w - radius) {
                *d = *s;
            } else {
                int32_t dist = rr_distance(px * FP + FP / 2, fy, cx, cy, hw, hh, r);
                int cov = iclamp(FP / 2 - dist, 0, 255);
                if (cov) *d = blend(*d, *s, (uint32_t)cov);
            }
            d++;
            s++;
        }
    }
}

void gfx_blit_scaled(const gfx_surface_t* src, int x, int y, int w, int h) {
    if (!target || !src || w <= 0 || h <= 0) return;
    int ax = x + state.ox, ay = y + state.oy;
    int x0 = imax(ax, state.clip.x), y0 = imax(ay, state.clip.y);
    int x1 = imin(ax + w, state.clip.x + state.clip.w);
    int y1 = imin(ay + h, state.clip.y + state.clip.h);
    if (x0 >= x1 || y0 >= y1) return;

    for (int py = y0; py < y1; py++) {
        int sy = (py - ay) * src->h / h;
        const uint32_t* s = &src->pixels[sy * src->pitch];
        uint32_t* d = &target->pixels[py * target->pitch + x0];
        for (int px = x0; px < x1; px++) {
            *d++ = 0xFF000000u | s[(px - ax) * src->w / w];
        }
    }
}

void gfx_copy_region(gfx_surface_t* dst, const gfx_surface_t* src,
                     int x, int y, int w, int h) {
    if (!dst || !src) return;
    int x0 = imax(x, 0), y0 = imax(y, 0);
    int x1 = imin(x + w, imin(dst->w, src->w));
    int y1 = imin(y + h, imin(dst->h, src->h));
    if (x0 >= x1 || y0 >= y1) return;
    for (int py = y0; py < y1; py++) {
        kmemcpy(&dst->pixels[py * dst->pitch + x0],
                &src->pixels[py * src->pitch + x0],
                (uint32_t)(x1 - x0) * 4u);
    }
}

/* --- text -------------------------------------------------------------- */

static const uifont_glyph_t* glyph_for(const uifont_t* font, unsigned char c) {
    if (c < font->first || c > font->last) return 0;
    return &font->glyphs[c - font->first];
}

int gfx_text_width_n(const uifont_t* font, const char* s, int n) {
    int width = 0;
    for (int i = 0; (n < 0 || i < n) && s[i]; i++) {
        const uifont_glyph_t* g = glyph_for(font, (unsigned char)s[i]);
        if (g) width += g->advance;
    }
    return width;
}

int gfx_text_width(const uifont_t* font, const char* s) {
    return gfx_text_width_n(font, s, -1);
}

int gfx_text_n(const uifont_t* font, int x, int y, const char* s, int n,
               uint32_t color) {
    if (!target || !font || !s) return x;
    int pen = x + state.ox;
    int baseline = y + state.oy + font->ascent;
    uint32_t alpha = GFX_ALPHA(color);

    for (int i = 0; (n < 0 || i < n) && s[i]; i++) {
        const uifont_glyph_t* g = glyph_for(font, (unsigned char)s[i]);
        if (!g) continue;
        if (g->w && g->h) {
            int gx = pen + g->left;
            int gy = baseline - g->top;
            int x0 = imax(gx, state.clip.x), y0 = imax(gy, state.clip.y);
            int x1 = imin(gx + g->w, state.clip.x + state.clip.w);
            int y1 = imin(gy + g->h, state.clip.y + state.clip.h);
            for (int py = y0; py < y1; py++) {
                const uint8_t* cov =
                    &font->pixels[g->offset + (py - gy) * g->w + (x0 - gx)];
                uint32_t* d = &target->pixels[py * target->pitch + x0];
                for (int px = x0; px < x1; px++) {
                    uint32_t a = *cov++;
                    if (a) {
                        if (alpha != 255) a = a * alpha / 255;
                        *d = blend(*d, color, a);
                    }
                    d++;
                }
            }
        }
        pen += g->advance;
    }
    return pen - state.ox;
}

int gfx_text(const uifont_t* font, int x, int y, const char* s, uint32_t color) {
    return gfx_text_n(font, x, y, s, -1, color);
}

void gfx_text_center(const uifont_t* font, int x, int y, int w, const char* s,
                     uint32_t color) {
    gfx_text(font, x + (w - gfx_text_width(font, s)) / 2, y, s, color);
}

void gfx_text_right(const uifont_t* font, int x, int y, int w, const char* s,
                    uint32_t color) {
    gfx_text(font, x + w - gfx_text_width(font, s), y, s, color);
}

void gfx_text_ellipsized(const uifont_t* font, int x, int y, int max_w,
                         const char* s, uint32_t color) {
    if (gfx_text_width(font, s) <= max_w) {
        gfx_text(font, x, y, s, color);
        return;
    }
    int ellipsis = gfx_text_width(font, UI_ELLIPSIS);
    int width = 0, n = 0;
    while (s[n]) {
        const uifont_glyph_t* g = glyph_for(font, (unsigned char)s[n]);
        int advance = g ? g->advance : 0;
        if (width + advance + ellipsis > max_w) break;
        width += advance;
        n++;
    }
    int pen = gfx_text_n(font, x, y, s, n, color);
    gfx_text(font, pen, y, UI_ELLIPSIS, color);
}

void gfx_mono_char(int x, int y, char c, uint32_t color) {
    if (!target) return;
    if (c < FONT8X16_FIRST_CHAR || c > FONT8X16_LAST_CHAR) return;
    const uint8_t* rows = font8x16[(unsigned char)c - FONT8X16_FIRST_CHAR];

    int ax = x + state.ox, ay = y + state.oy;
    int x0 = imax(ax, state.clip.x), y0 = imax(ay, state.clip.y);
    int x1 = imin(ax + GFX_MONO_W, state.clip.x + state.clip.w);
    int y1 = imin(ay + GFX_MONO_H, state.clip.y + state.clip.h);
    uint32_t alpha = GFX_ALPHA(color);

    for (int py = y0; py < y1; py++) {
        uint8_t bits = rows[py - ay];
        if (!bits) continue;
        uint32_t* d = &target->pixels[py * target->pitch + x0];
        for (int px = x0; px < x1; px++) {
            if (bits & (0x80 >> (px - ax))) *d = blend(*d, color, alpha);
            d++;
        }
    }
}

void gfx_mono_text(int x, int y, const char* s, uint32_t color) {
    while (*s) {
        gfx_mono_char(x, y, *s++, color);
        x += GFX_MONO_W;
    }
}

/* --- presenting -------------------------------------------------------- */

void gfx_present(const gfx_surface_t* s, int x, int y, int w, int h) {
    if (!s) return;
    int x0 = imax(x, 0), y0 = imax(y, 0);
    int x1 = imin(x + w, s->w), y1 = imin(y + h, s->h);
    if (x0 >= x1 || y0 >= y1) return;
    fb_blit((uint32_t)x0, (uint32_t)y0, (uint32_t)(x1 - x0), (uint32_t)(y1 - y0),
            &s->pixels[y0 * s->pitch + x0], (uint32_t)s->pitch);
}

/* --- rect helpers ------------------------------------------------------ */

int gfx_rect_contains(const gfx_rect_t* r, int x, int y) {
    return x >= r->x && y >= r->y && x < r->x + r->w && y < r->y + r->h;
}

void gfx_rect_union(gfx_rect_t* into, const gfx_rect_t* other) {
    if (other->w <= 0 || other->h <= 0) return;
    if (into->w <= 0 || into->h <= 0) { *into = *other; return; }
    int x0 = imin(into->x, other->x);
    int y0 = imin(into->y, other->y);
    int x1 = imax(into->x + into->w, other->x + other->w);
    int y1 = imax(into->y + into->h, other->y + other->h);
    into->x = x0;
    into->y = y0;
    into->w = x1 - x0;
    into->h = y1 - y0;
}

int gfx_rect_intersects(const gfx_rect_t* a, const gfx_rect_t* b) {
    return a->x < b->x + b->w && b->x < a->x + a->w &&
           a->y < b->y + b->h && b->y < a->y + a->h;
}
