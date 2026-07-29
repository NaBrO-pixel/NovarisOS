/* app_monitor.c - Activity Monitor: what the machine is actually doing.
 *
 * Every number in here is read live from the subsystem that owns it - the
 * physical frame allocator, the heap's own block list, the PIT's tick
 * counter, the window manager's window table. The "processes" it lists
 * are open windows, because on this kernel that is what the live,
 * user-facing things are; a scheduler process only exists for the
 * duration of `multitask`.
 */

#include "apps.h"
#include "uikit.h"
#include "gfx.h"
#include "pmm.h"
#include "pit.h"
#include "kheap.h"
#include "cpu.h"
#include "framebuffer.h"
#include "kstring.h"

#define HISTORY_LEN 96
#define CARD_H      108

static uint8_t history[HISTORY_LEN];
static int history_pos;
static int history_filled;
static uint32_t last_sample_tick;

static int memory_percent(void) {
    uint32_t total = pmm_total_frames();
    if (!total) return 0;
    return (int)((total - pmm_free_frames()) * 100 / total);
}

static void card(int x, int y, int w, int h, const char* title) {
    gfx_round_rect(x, y + 1, w, h, 10, GFX_ARGB(0x10, 0, 0, 0));
    gfx_round_rect(x, y, w, h, 10, UI_CARD);
    gfx_round_frame(x, y, w, h, 10, 1, GFX_ARGB(0x18, 0, 0, 0));
    gfx_text(&uifont_small, x + 14, y + 11, title, UI_TEXT_FAINT);
}

static void draw_graph(int x, int y, int w, int h) {
    gfx_round_rect(x, y, w, h, 8, GFX_RGB(0xF7, 0xF7, 0xFA));
    gfx_round_frame(x, y, w, h, 8, 1, GFX_ARGB(0x14, 0, 0, 0));

    /* Gridlines at 25% intervals. */
    for (int i = 1; i < 4; i++) {
        gfx_fill(x + 6, y + h * i / 4, w - 12, 1, GFX_RGB(0xE6, 0xE6, 0xEC));
    }

    int samples = history_filled ? HISTORY_LEN : history_pos;
    if (samples < 2) return;
    int step = (w - 12) / HISTORY_LEN;
    if (step < 1) step = 1;

    for (int i = 0; i < samples; i++) {
        /* Oldest sample first, so the graph scrolls left as it fills. */
        int index = (history_pos - samples + i + HISTORY_LEN * 2) % HISTORY_LEN;
        int value = history[index];
        int bar_h = (h - 12) * value / 100;
        if (bar_h < 1) bar_h = 1;
        int bx = x + 6 + i * step;
        gfx_fill(bx, y + h - 6 - bar_h, step > 1 ? step - 1 : 1, bar_h,
                 GFX_ARGB(0xCC, 0x00, 0x7A, 0xFF));
    }
}

static void monitor_paint(window_t* win) {
    int cw = wm_client_w(win), ch = wm_client_h(win);
    gfx_fill(0, 0, cw, ch, GFX_RGB(0xF2, 0xF2, 0xF5));

    char buf[80], num[16];
    int margin = 16;
    int card_w = (cw - margin * 3) / 2;

    /* --- memory card --- */
    card(margin, margin, card_w, CARD_H, "MEMORY");
    uint32_t total_kb = pmm_total_frames() * 4;
    uint32_t used_kb = total_kb - pmm_free_frames() * 4;
    int percent = memory_percent();

    ui_fmt_uint(num, used_kb / 1024);
    kstrcpy(buf, num);
    kstrcat(buf, " MB");
    gfx_text(&uifont_title, margin + 14, margin + 28, buf, UI_TEXT);
    ui_fmt_uint(num, total_kb / 1024);
    kstrcpy(buf, "of ");
    kstrcat(buf, num);
    kstrcat(buf, " MB used");
    gfx_text(&uifont_small, margin + 14, margin + 60, buf, UI_TEXT_DIM);
    ui_progress(margin + 14, margin + 80, card_w - 28, 8, percent, UI_ACCENT);

    /* --- uptime card --- */
    int x2 = margin * 2 + card_w;
    card(x2, margin, card_w, CARD_H, "UPTIME");
    uint32_t ticks = pit_get_ticks();
    ui_fmt_duration(buf, ticks / 100);
    gfx_text(&uifont_title, x2 + 14, margin + 28, buf, UI_TEXT);
    ui_fmt_grouped(num, ticks);
    kstrcpy(buf, num);
    kstrcat(buf, " timer ticks at 100 Hz");
    gfx_text(&uifont_small, x2 + 14, margin + 60, buf, UI_TEXT_DIM);
    gfx_text_ellipsized(&uifont_small, x2 + 14, margin + 78, card_w - 28,
                        cpu_brand(), UI_TEXT_FAINT);

    /* --- history graph --- */
    int gy = margin * 2 + CARD_H;
    card(margin, gy, cw - margin * 2, 116, "PHYSICAL MEMORY IN USE");
    draw_graph(margin + 12, gy + 28, cw - margin * 2 - 24, 78);

    /* --- window table --- */
    int ty = gy + 116 + margin;
    gfx_text(&uifont_bold, margin + 2, ty, "Open Windows", UI_TEXT);
    ty += 22;
    gfx_fill(margin, ty, cw - margin * 2, 1, UI_SEPARATOR);
    ty += 6;

    gfx_text(&uifont_small, margin + 8, ty, "APPLICATION", UI_TEXT_FAINT);
    gfx_text(&uifont_small, margin + 130, ty, "WINDOW", UI_TEXT_FAINT);
    gfx_text_right(&uifont_small, margin, ty, cw - margin * 2 - 8, "SURFACE",
                   UI_TEXT_FAINT);
    ty += 18;

    uint32_t surface_bytes = 0;
    for (int i = wm_count() - 1; i >= 0; i--) {
        window_t* w = wm_at(i);
        if (!w || !w->open) continue;
        uint32_t bytes = (uint32_t)w->w * (uint32_t)w->h * 4u;
        surface_bytes += bytes;
        if (ty > ch - 40) continue;

        gfx_text(&uifont_regular, margin + 8, ty, w->app ? w->app->name : "?",
                 w->minimized ? UI_TEXT_FAINT : UI_TEXT);
        gfx_text_ellipsized(&uifont_regular, margin + 130, ty, cw - margin - 250,
                            w->title, UI_TEXT_DIM);
        ui_fmt_bytes(buf, bytes);
        gfx_text_right(&uifont_small, margin, ty + 2, cw - margin * 2 - 8, buf,
                       UI_TEXT_DIM);
        ty += 20;
    }

    /* --- footer: the heap, which is what all of the above is drawn out of --- */
    uint32_t heap_used = 0, heap_total = 0;
    kheap_stats(&heap_used, &heap_total);
    ui_fmt_bytes(buf, heap_used);
    kstrcpy(num, " of ");
    kstrcat(buf, num);
    char total_str[24];
    ui_fmt_bytes(total_str, heap_total);
    kstrcat(buf, total_str);
    kstrcat(buf, " kernel heap in use");
    gfx_fill(margin, ch - 28, cw - margin * 2, 1, UI_SEPARATOR);
    gfx_text(&uifont_small, margin + 8, ch - 20, buf, UI_TEXT_FAINT);

    ui_fmt_bytes(total_str, surface_bytes);
    kstrcpy(buf, total_str);
    kstrcat(buf, " in window surfaces");
    gfx_text_right(&uifont_small, margin, ch - 20, cw - margin * 2 - 8, buf,
                   UI_TEXT_FAINT);
}

static void monitor_tick(window_t* win) {
    uint32_t now = pit_get_ticks();
    if (now - last_sample_tick < 100) {   /* one sample a second */
        return;
    }
    last_sample_tick = now;
    history[history_pos] = (uint8_t)memory_percent();
    history_pos = (history_pos + 1) % HISTORY_LEN;
    if (history_pos == 0) history_filled = 1;
    wm_invalidate(win);
}

const app_t app_monitor = {
    "Activity Monitor", monitor_paint, 0, 0, monitor_tick, 0,
};

window_t* monitor_open(void) {
    window_t* existing = wm_find_by_app(&app_monitor);
    if (existing) {
        wm_focus(existing);
        return existing;
    }
    window_t* win = wm_open(&app_monitor, "Activity Monitor", 620, 500, 0);
    if (win) {
        win->bg = GFX_RGB(0xF2, 0xF2, 0xF5);
        win->min_w = 480;
        win->min_h = 380;
    }
    return win;
}
