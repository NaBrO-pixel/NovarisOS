/* app_about.c - "About This Novaris", and the alert panel.
 *
 * The About window is the one place the OS introduces itself, so it says
 * true things it had to go and find out: the processor's own brand string
 * from CPUID, the memory the bootloader reported, the video mode VBE
 * actually granted. Nothing here is a constant pretending to be a fact.
 */

#include "apps.h"
#include "uikit.h"
#include "gfx.h"
#include "cpu.h"
#include "pmm.h"
#include "pit.h"
#include "kheap.h"
#include "framebuffer.h"
#include "kstring.h"
#include "win32.h"

#define ABOUT_W 460
#define ABOUT_H 476

typedef struct {
    int hover_button;   /* 0 = System Report, 1 = Software Update, -1 = none */
    int pressed_button;
} about_state_t;

static about_state_t about_state;

static void about_paint(window_t* win) {
    int cw = wm_client_w(win);
    gfx_fill(0, 0, cw, wm_client_h(win), GFX_RGB(0xF2, 0xF2, 0xF5));

    /* The mark, with a soft glow behind it. */
    int cx = cw / 2, cy = 78;
    gfx_circle(cx, cy, 44, GFX_ARGB(0x18, 0x5A, 0x7C, 0xFF));
    gfx_circle(cx, cy, 30, GFX_ARGB(0x22, 0x5A, 0x7C, 0xFF));
    gfx_sparkle(cx, cy, 34, GFX_RGB(0x3E, 0x6F, 0xF0));
    gfx_sparkle(cx, cy, 20, GFX_RGB(0xBB, 0xD4, 0xFF));

    gfx_text_center(&uifont_title, 0, cy + 46, cw, "Novaris", UI_TEXT);
    gfx_text_center(&uifont_regular, 0, cy + 76, cw,
                    "Version 0.11 " UI_BULLET " Milestone 11", UI_TEXT_DIM);

    int y = cy + 108;
    int label_w = 132;
    int x = 34;
    char buf[80], num[16];

    ui_key_value(x, y, label_w, "Processor", cpu_brand());
    y += 24;

    uint32_t total_kb = pmm_total_frames() * 4;
    uint32_t free_kb = pmm_free_frames() * 4;
    ui_fmt_uint(num, total_kb / 1024);
    kstrcpy(buf, num);
    kstrcat(buf, " MB (");
    ui_fmt_uint(num, free_kb / 1024);
    kstrcat(buf, num);
    kstrcat(buf, " MB free)");
    ui_key_value(x, y, label_w, "Memory", buf);
    y += 24;

    ui_fmt_uint(num, fb_width());
    kstrcpy(buf, num);
    kstrcat(buf, " " UI_BULLET " ");
    ui_fmt_uint(num, fb_height());
    kstrcat(buf, num);
    kstrcat(buf, "  32-bit color");
    ui_key_value(x, y, label_w, "Graphics", buf);
    y += 24;

    ui_key_value(x, y, label_w, "Startup Disk", "initrd (GRUB module)");
    y += 24;

    uint32_t apis = 0;
    for (uint32_t m = 0; m < win32_module_count(); m++) apis += win32_module_exports(m);
    ui_fmt_uint(num, apis);
    kstrcpy(buf, num);
    kstrcat(buf, " Win32 APIs emulated");
    ui_key_value(x, y, label_w, "Compatibility", buf);
    y += 32;

    ui_separator(x, y, cw - x * 2);
    y += 18;

    gfx_text_center(&uifont_small, 0, y, cw,
                    "A 32-bit x86 kernel written from scratch: own GDT, IDT,",
                    UI_TEXT_FAINT);
    gfx_text_center(&uifont_small, 0, y + 16, cw,
                    "paging, scheduler, filesystem, compositor and window manager.",
                    UI_TEXT_FAINT);

    int by = wm_client_h(win) - 52;
    ui_button(cw / 2 - 168, by, 160, 30, "System Report" UI_ELLIPSIS,
              about_state.pressed_button == 0 ? UI_BTN_PRESSED :
              (about_state.hover_button == 0 ? UI_BTN_HOVER : UI_BTN_NORMAL));
    ui_button(cw / 2 + 8, by, 160, 30, "Software Update" UI_ELLIPSIS,
              about_state.pressed_button == 1 ? UI_BTN_PRESSED :
              (about_state.hover_button == 1 ? UI_BTN_HOVER : UI_BTN_NORMAL));
}

static int about_button_at(window_t* win, int x, int y) {
    int cw = wm_client_w(win);
    int by = wm_client_h(win) - 52;
    if (ui_hit(cw / 2 - 168, by, 160, 30, x, y)) return 0;
    if (ui_hit(cw / 2 + 8, by, 160, 30, x, y)) return 1;
    return -1;
}

static void about_mouse(window_t* win, int x, int y, int kind, int buttons,
                        int wheel) {
    (void)buttons; (void)wheel;
    int over = about_button_at(win, x, y);

    if (kind == WM_MOUSE_MOVE || kind == WM_MOUSE_DRAG) {
        if (over != about_state.hover_button) {
            about_state.hover_button = over;
            wm_invalidate(win);
        }
        return;
    }
    if (kind == WM_MOUSE_DOWN || kind == WM_MOUSE_DOUBLE) {
        about_state.pressed_button = over;
        wm_invalidate(win);
        return;
    }
    if (kind == WM_MOUSE_UP) {
        int pressed = about_state.pressed_button;
        about_state.pressed_button = -1;
        wm_invalidate(win);
        if (pressed >= 0 && pressed == over) {
            if (pressed == 0) monitor_open();
            else alert_open("Novaris is up to date",
                            "There is no software update service. This is the "
                            "newest version, because it is the only version.");
        }
    }
}

const app_t app_about = {
    "About This Novaris", about_paint, 0, about_mouse, 0, 0,
};

window_t* about_open(void) {
    window_t* existing = wm_find_by_app(&app_about);
    if (existing) {
        wm_focus(existing);
        return existing;
    }
    about_state.hover_button = -1;
    about_state.pressed_button = -1;
    window_t* win = wm_open(&app_about, "About This Novaris", ABOUT_W, ABOUT_H, 0);
    if (win) {
        win->resizable = 0;
        win->bg = GFX_RGB(0xF2, 0xF2, 0xF5);
    }
    return win;
}

/* --- alert ------------------------------------------------------------- */

#define ALERT_MAX_TEXT 240

typedef struct {
    char heading[80];
    char message[ALERT_MAX_TEXT];
    int hover_ok, pressed_ok;
} alert_state_t;

/* Alerts are small and rare; two at once is already more than this UI
 * wants to show, and a fixed pool avoids an allocation on a path that
 * might be reporting that allocation failed. */
static alert_state_t alert_pool[2];

/* Word-wraps `text` into the given width, drawing each line centered. */
static int draw_wrapped(const char* text, int x, int y, int w) {
    int line_start = 0, last_space = -1, i = 0;
    int lines = 0;
    char line[ALERT_MAX_TEXT];

    while (text[i]) {
        if (text[i] == ' ') last_space = i;
        int len = i - line_start + 1;
        kmemcpy(line, text + line_start, (uint32_t)len);
        line[len] = '\0';
        if (gfx_text_width(&uifont_regular, line) > w && last_space > line_start) {
            int cut = last_space - line_start;
            kmemcpy(line, text + line_start, (uint32_t)cut);
            line[cut] = '\0';
            gfx_text_center(&uifont_regular, x, y + lines * 18, w, line, UI_TEXT_DIM);
            lines++;
            line_start = last_space + 1;
            last_space = -1;
        }
        i++;
    }
    if (line_start < i) {
        kmemcpy(line, text + line_start, (uint32_t)(i - line_start));
        line[i - line_start] = '\0';
        gfx_text_center(&uifont_regular, x, y + lines * 18, w, line, UI_TEXT_DIM);
        lines++;
    }
    return lines;
}

static void alert_paint(window_t* win) {
    alert_state_t* st = (alert_state_t*)win->data;
    int cw = wm_client_w(win), ch = wm_client_h(win);
    gfx_fill(0, 0, cw, ch, GFX_RGB(0xF2, 0xF2, 0xF5));

    gfx_sparkle(cw / 2, 34, 20, GFX_RGB(0x3E, 0x6F, 0xF0));
    gfx_text_center(&uifont_bold, 0, 64, cw, st->heading, UI_TEXT);
    draw_wrapped(st->message, 26, 90, cw - 52);

    ui_button(cw / 2 - 45, ch - 46, 90, 28, "OK",
              st->pressed_ok ? UI_BTN_PRESSED : UI_BTN_DEFAULT);
}

static void alert_mouse(window_t* win, int x, int y, int kind, int buttons,
                        int wheel) {
    (void)buttons; (void)wheel;
    alert_state_t* st = (alert_state_t*)win->data;
    int cw = wm_client_w(win), ch = wm_client_h(win);
    int over = ui_hit(cw / 2 - 45, ch - 46, 90, 28, x, y);

    if (kind == WM_MOUSE_DOWN || kind == WM_MOUSE_DOUBLE) {
        st->pressed_ok = over;
        wm_invalidate(win);
    } else if (kind == WM_MOUSE_UP) {
        int was = st->pressed_ok;
        st->pressed_ok = 0;
        if (was && over) wm_close(win);
        else wm_invalidate(win);
    } else if (kind == WM_MOUSE_MOVE) {
        if (over != st->hover_ok) {
            st->hover_ok = over;
            wm_invalidate(win);
        }
    }
}

static void alert_key(window_t* win, const key_event_t* ev) {
    if (ev->pressed && (ev->code == KEY_ENTER || ev->code == KEY_ESCAPE)) {
        wm_close(win);
    }
}

static void alert_closed(window_t* win) {
    alert_state_t* st = (alert_state_t*)win->data;
    if (st) st->heading[0] = '\0';   /* returns the slot to the pool */
}

const app_t app_alert = {
    "Novaris", alert_paint, alert_key, alert_mouse, 0, alert_closed,
};

window_t* alert_open(const char* heading, const char* message) {
    alert_state_t* st = 0;
    for (uint32_t i = 0; i < sizeof(alert_pool) / sizeof(alert_pool[0]); i++) {
        if (!alert_pool[i].heading[0]) { st = &alert_pool[i]; break; }
    }
    if (!st) return 0;

    kstrlcpy(st->heading, heading, sizeof(st->heading));
    kstrlcpy(st->message, message, sizeof(st->message));
    st->hover_ok = 0;
    st->pressed_ok = 0;

    window_t* win = wm_open(&app_alert, "Novaris", 400, 210, st);
    if (win) {
        win->resizable = 0;
        win->bg = GFX_RGB(0xF2, 0xF2, 0xF5);
    } else {
        st->heading[0] = '\0';
    }
    return win;
}
