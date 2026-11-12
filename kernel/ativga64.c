/* ativga64.c - a display driver for ATI's Rage 128 and Radeon (Milestone 112).
 *
 * The first GPU this kernel drives itself rather than inherits from the
 * firmware: it reads the mode it was left in, sets another, and flips
 * the screen between buffers in video memory. The hardware is QEMU's
 * ati-vga, which emulates a Rage 128 Pro or a Radeon 7000 - AMD's
 * lineage, twenty-five years back - because it is the one ATI/AMD part
 * QEMU can emulate, and a driver that cannot be run is a driver that has
 * not been tested.
 *
 * Register offsets and bits are from Linux's radeon_reg.h:
 *
 *   Copyright 2000 ATI Technologies Inc., Markham, Ontario, and
 *                  VA Linux Systems Inc., Fremont, California.
 *   All Rights Reserved.
 *
 *   Permission is hereby granted, free of charge, to any person obtaining
 *   a copy of this software and associated documentation files (the
 *   "Software"), to deal in the Software without restriction, including
 *   without limitation on the rights to use, copy, modify, merge,
 *   publish, distribute, sublicense, and/or sell copies of the Software,
 *   and to permit persons to whom the Software is furnished to do so,
 *   subject to the following conditions:
 *
 *   The above copyright notice and this permission notice (including the
 *   next paragraph) shall be included in all copies or substantial
 *   portions of the Software.
 *
 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 *   EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 *   MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 *   NON-INFRINGEMENT.  IN NO EVENT SHALL ATI, VA LINUX SYSTEMS AND/OR
 *   THEIR SUPPLIERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 *   WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 *   DEALINGS IN THE SOFTWARE.
 *
 * What this does not do, and a real card would need: program the pixel
 * clock PLL, so a new mode on real hardware keeps the old refresh
 * timing; the second CRTC; the 2D engine; interrupts. QEMU's display
 * takes its geometry from the CRTC registers and ignores the clock, so
 * what is here is exactly what can be checked.
 */

#include "ativga64.h"
#include "gpu64.h"
#include "serial64.h"

#define RADEON_MM_INDEX              0x0000
#define RADEON_MM_DATA               0x0004
#define RADEON_CRTC_GEN_CNTL         0x0050
#define   RADEON_CRTC_PIX_WIDTH_MASK   (7u << 8)
#define   RADEON_CRTC_PIX_WIDTH_32BPP  (6u << 8)
#define   RADEON_CRTC_CUR_EN           (1u << 16)
#define   RADEON_CRTC_EXT_DISP_EN      (1u << 24)
#define   RADEON_CRTC_EN               (1u << 25)
#define RADEON_CRTC_EXT_CNTL         0x0054
#define   RADEON_CRTC_DISPLAY_DIS      (1u << 10)
#define   RADEON_CRTC_CRT_ON           (1u << 15)
#define RADEON_CONFIG_MEMSIZE        0x00f8
#define RADEON_CRTC_H_TOTAL_DISP     0x0200
#define RADEON_CRTC_H_SYNC_STRT_WID  0x0204
#define RADEON_CRTC_V_TOTAL_DISP     0x0208
#define RADEON_CRTC_V_SYNC_STRT_WID  0x020c
#define RADEON_CRTC_OFFSET           0x0224
#define RADEON_CRTC_OFFSET_CNTL      0x0228
#define RADEON_CRTC_PITCH            0x022c

static const gpu64_t* card;
static volatile uint32_t* regs;

static uint32_t rd(uint32_t off) { return regs[off / 4]; }
static void wr(uint32_t off, uint32_t v) { regs[off / 4] = v; }

int ativga64_probe(void) {
    card = gpu64_find_kind(GPU64_ATI_OLD);
    regs = 0;
    if (!card) return 0;
    /* The registers are BAR 2 on both models QEMU emulates; BAR 0 is the
     * video memory and BAR 1 the same registers as I/O ports. */
    if (card->bar[2].io || card->bar[2].size < 0x4000) return 0;
    regs = gpu64_map(card, 2, 0, 0x4000);
    return regs != 0;
}

uint64_t ativga64_vram_size(void) {
    return regs ? rd(RADEON_CONFIG_MEMSIZE) : 0;
}

uint64_t ativga64_vram_phys(void) {
    return card ? card->bar[0].base : 0;
}

int ativga64_get_mode(ativga64_mode_t* m) {
    uint32_t gen, h, v;

    if (!regs) return -19;                               /* -ENODEV */
    gen = rd(RADEON_CRTC_GEN_CNTL);
    h = rd(RADEON_CRTC_H_TOTAL_DISP);
    v = rd(RADEON_CRTC_V_TOTAL_DISP);
    m->enabled = (gen & RADEON_CRTC_EN) && (gen & RADEON_CRTC_EXT_DISP_EN);
    m->width = (((h >> 16) & 0x1FF) + 1) * 8;
    m->height = ((v >> 16) & 0x7FF) + 1;
    switch (gen & RADEON_CRTC_PIX_WIDTH_MASK) {
    case 2u << 8: m->bpp = 8; break;
    case 3u << 8: m->bpp = 15; break;
    case 4u << 8: m->bpp = 16; break;
    case 5u << 8: m->bpp = 24; break;
    case 6u << 8: m->bpp = 32; break;
    default:      m->bpp = 0; break;
    }
    /* The pitch register counts in units of eight pixels. */
    m->pitch = (rd(RADEON_CRTC_PITCH) & 0x7FF) * 8 * ((m->bpp + 7) / 8);
    m->offset = rd(RADEON_CRTC_OFFSET) & 0x07FFFFFF;
    return 0;
}

/* Blanking of a fifth of the width and a twentieth of the height, sync in
 * the middle of it: plausible numbers for a monitor that reads them, and
 * the display fields are all QEMU looks at. */
int ativga64_set_mode(uint32_t width, uint32_t height, uint32_t pitch_pixels, uint32_t offset) {
    uint32_t gen, ext, htotal, vtotal;

    if (!regs) return -19;
    if (!width || width % 8 || width > 4096 || !height || height > 2048) return -22;
    if (pitch_pixels < width || pitch_pixels % 8 || pitch_pixels / 8 > 0x7FF) return -22;
    if ((uint64_t)offset + (uint64_t)pitch_pixels * 4 * height > ativga64_vram_size()) return -22;

    htotal = width + width / 5;
    vtotal = height + height / 20 + 1;
    gen = rd(RADEON_CRTC_GEN_CNTL);
    ext = rd(RADEON_CRTC_EXT_CNTL);

    /* Off, reprogrammed, on again: the controller takes a new geometry
     * when it is switched on. */
    wr(RADEON_CRTC_GEN_CNTL, gen & ~(RADEON_CRTC_EN | RADEON_CRTC_EXT_DISP_EN));
    wr(RADEON_CRTC_H_TOTAL_DISP, ((width / 8 - 1) << 16) | (htotal / 8 - 1));
    wr(RADEON_CRTC_H_SYNC_STRT_WID, ((width + width / 20) & 0x1FFF) | (((width / 20 / 8) & 0x3F) << 16));
    wr(RADEON_CRTC_V_TOTAL_DISP, ((height - 1) << 16) | (vtotal - 1));
    wr(RADEON_CRTC_V_SYNC_STRT_WID, ((height + 1) & 0xFFF) | (3u << 16));
    wr(RADEON_CRTC_PITCH, pitch_pixels / 8);
    wr(RADEON_CRTC_OFFSET_CNTL, 0);
    wr(RADEON_CRTC_OFFSET, offset);
    wr(RADEON_CRTC_EXT_CNTL, (ext & ~RADEON_CRTC_DISPLAY_DIS) | RADEON_CRTC_CRT_ON);
    wr(RADEON_CRTC_GEN_CNTL, (gen & ~(RADEON_CRTC_PIX_WIDTH_MASK | RADEON_CRTC_CUR_EN)) |
                             RADEON_CRTC_PIX_WIDTH_32BPP | RADEON_CRTC_EN | RADEON_CRTC_EXT_DISP_EN);
    return 0;
}

/* Where the screen is scanned out from. A real card latches the new
 * offset at the next vertical blank; QEMU's takes it when the display is
 * (re)enabled, which writing CRTC_EXT_CNTL with the display on is - and
 * on a real card that write changes nothing. */
int ativga64_flip(uint32_t offset) {
    ativga64_mode_t m;

    if (!regs) return -19;
    if (ativga64_get_mode(&m) || !m.enabled) return -22;
    if ((uint64_t)offset + (uint64_t)m.pitch * m.height > ativga64_vram_size()) return -22;
    if (offset % m.pitch) return -22;
    wr(RADEON_CRTC_OFFSET, offset);
    wr(RADEON_CRTC_EXT_CNTL, (rd(RADEON_CRTC_EXT_CNTL) & ~RADEON_CRTC_DISPLAY_DIS) | RADEON_CRTC_CRT_ON);
    return 0;
}
