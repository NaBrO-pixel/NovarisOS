/* wm_host_test.c - drives the real window manager and desktop shell on the
 * host, with everything below the compositor stubbed out.
 *
 * The point is the same as the other host tests: link the *actual* kernel
 * sources rather than a reimplementation, so a failure names the thing
 * that broke. What's specific to this one is that window management is
 * mostly geometry, and geometry is exactly the kind of code that looks
 * right in a screenshot and is off by two pixels - "the right edge stayed
 * where it was during a left-edge resize" is a claim a test can make and
 * an eyeball can't.
 *
 * It also renders frames to PPM (`--shots DIR`), which is how the chrome
 * gets looked at without booting anything.
 *
 * Stubs: the heap becomes malloc, the framebuffer swallows its blits, the
 * RTC returns a fixed instant so the taskbar clock is reproducible, the
 * initrd is three invented files, and the apps are one paint routine
 * wearing six hats. Nothing above those is faked - gfx.c, wm.c, icons.c
 * and desktop.c are the shipping sources.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "gfx.h"
#include "wm.h"
#include "apps.h"
#include "uikit.h"
#include "kheap.h"
#include "console.h"
#include "rtc.h"
#include "vfs.h"

/* --- stubs -------------------------------------------------------------- */

void* kmalloc(size_t n) { return malloc(n); }
void  kfree(void* p) { free(p); }
void  kheap_stats(uint32_t* used, uint32_t* total) {
    if (used) *used = 0;
    if (total) *total = 0;
}

#define SCREEN_W 1280
#define SCREEN_H 800

uint32_t fb_width(void) { return SCREEN_W; }
uint32_t fb_height(void) { return SCREEN_H; }
uint32_t fb_pitch(void) { return SCREEN_W * 4; }
uint32_t fb_bpp(void) { return 32; }
int  fb_available(void) { return 1; }
void fb_clear(uint32_t color) { (void)color; }
void fb_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
             const uint32_t* pixels, uint32_t pitch) {
    (void)x; (void)y; (void)w; (void)h; (void)pixels; (void)pitch;
}

/* Frozen, so the taskbar clock renders the same string every run. */
static uint32_t host_ticks = 1000;
uint32_t pit_get_ticks(void) { return host_ticks; }
uint32_t pit_get_seconds(void) { return host_ticks / 100; }

void rtc_read(rtc_time_t* out) {
    out->year = 2026; out->month = 7; out->day = 29;
    out->hour = 14; out->minute = 35; out->second = 0; out->weekday = 3;
}
uint32_t rtc_unix_time(void) { return 0; }
int32_t rtc_days_from_civil(int32_t y, uint32_t m, uint32_t d) {
    (void)y; (void)m; (void)d;
    return 0;
}

static vfs_node_t host_files[3];
vfs_node_t* vfs_root = 0;
uint32_t vfs_read(vfs_node_t* n, uint32_t off, uint32_t size, uint8_t* buf) {
    (void)n; (void)off; (void)size; (void)buf;
    return 0;
}
vfs_node_t* vfs_readdir(vfs_node_t* n, uint32_t i) {
    (void)n;
    return i < 3 ? &host_files[i] : 0;
}
vfs_node_t* vfs_finddir(vfs_node_t* n, const char* name) {
    (void)n; (void)name;
    return 0;
}

void console_set_sink(console_sink_t sink) { (void)sink; }
int  console_has_framebuffer(void) { return 1; }
void console_clear(void) { }
void terminal_writestring(const char* s) { (void)s; }
void shell_init(void) { }
void shell_run_line(const char* line) { (void)line; }

void mouse_set_bounds(int w, int h) { (void)w; (void)h; }
int  mouse_poll_event(mouse_event_t* out) { (void)out; return 0; }
int  mouse_x(void) { return 0; }
int  mouse_y(void) { return 0; }
uint8_t mouse_buttons(void) { return 0; }
int  keyboard_poll_event(key_event_t* out) { (void)out; return 0; }
uint8_t keyboard_mods(void) { return 0; }

/* --- stand-in apps ------------------------------------------------------ */

static void demo_paint(window_t* win) {
    int w = wm_client_w(win), h = wm_client_h(win);
    if (win->app == &app_terminal) {
        gfx_fill(0, 0, w, h, GFX_RGB(0x0C, 0x0C, 0x0C));
        gfx_mono_text(10, 10, "novaris> winapi", GFX_RGB(0x7C, 0xF6, 0x8A));
        gfx_mono_text(10, 30, "kernel32.dll   61 exports", GFX_RGB(0xDD, 0xDD, 0xDD));
        gfx_mono_text(10, 46, "user32.dll     24 exports", GFX_RGB(0xDD, 0xDD, 0xDD));
        gfx_mono_text(10, 62, "msvcrt.dll     94 exports", GFX_RGB(0xDD, 0xDD, 0xDD));
        gfx_mono_text(10, 86, "novaris> _", GFX_RGB(0x7C, 0xF6, 0x8A));
        return;
    }
    gfx_fill(0, 0, w, h, GFX_RGB(0xFF, 0xFF, 0xFF));
    gfx_fill(0, 0, w, 40, UI_PANEL);
    gfx_fill(0, 39, w, 1, UI_SEPARATOR);
    gfx_text(&uifont_regular, 14, 12, win->title, UI_TEXT);
    for (int i = 0; i < 6; i++) {
        int y = 52 + i * 26;
        if (i & 1) gfx_fill(0, y - 4, w, 26, UI_ROW_ALT);
        gfx_text(&uifont_regular, 20, y, "hello.txt", UI_TEXT);
        gfx_text_right(&uifont_small, 0, y + 1, w - 20, "1.2 KB", UI_TEXT_DIM);
    }
    ui_button(20, h - 46, 130, 30, "Task Manager", UI_BTN_NORMAL);
    ui_button(160, h - 46, 130, 30, "Open", UI_BTN_DEFAULT);
}

const app_t app_terminal = { "Terminal",      demo_paint, 0, 0, 0, 0 };
const app_t app_files    = { "File Explorer", demo_paint, 0, 0, 0, 0 };
const app_t app_monitor  = { "Task Manager",  demo_paint, 0, 0, 0, 0 };
const app_t app_about    = { "About Novaris", demo_paint, 0, 0, 0, 0 };
const app_t app_textview = { "TextView",      demo_paint, 0, 0, 0, 0 };
const app_t app_alert    = { "Novaris",       demo_paint, 0, 0, 0, 0 };

window_t* terminal_open(void) { return 0; }
void terminal_app_init(void) { }
void terminal_run(const char* command) { (void)command; }
window_t* files_open(void) { return 0; }
window_t* monitor_open(void) { return 0; }
window_t* about_open(void) { return 0; }
window_t* textview_open(const char* file) { (void)file; return 0; }
window_t* alert_open(const char* heading, const char* message) {
    (void)heading; (void)message;
    return 0;
}

/* The shell itself, statics and all - the test drives its internals
 * directly rather than through desktop_start(), which never returns. */
#include "../kernel/desktop.c"

/* --- harness ------------------------------------------------------------ */

static int failures;
static int checks;

#define CHECK(cond, ...) do {                     \
        checks++;                                 \
        if (!(cond)) {                            \
            printf("  FAIL: ");                   \
            printf(__VA_ARGS__);                  \
            printf("\n");                         \
            failures++;                           \
        }                                         \
    } while (0)

static void button_event(int x, int y, uint8_t pressed, uint8_t released,
                         uint8_t held) {
    mouse_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.x = (int16_t)x;
    ev.y = (int16_t)y;
    ev.pressed = pressed;
    ev.released = released;
    ev.buttons = held;
    handle_mouse(&ev);
}

static void move(int x, int y)    { button_event(x, y, 0, 0, 0); }
static void press(int x, int y)   { button_event(x, y, MOUSE_BUTTON_LEFT, 0, MOUSE_BUTTON_LEFT); }
static void drag(int x, int y)    { button_event(x, y, 0, 0, MOUSE_BUTTON_LEFT); }
static void release(int x, int y) { button_event(x, y, 0, MOUSE_BUTTON_LEFT, 0); }
static void click(int x, int y)   { press(x, y); release(x, y); }

/* wm.c keeps the hover state private, so this reads it back off the
 * pixels: if any caption button is lit, the strip it occupies is no
 * longer the flat caption color. */
static void render(void) {
    gfx_rect_t all = { 0, 0, screen_w, screen_h };
    compose(all);
}

static int caption_button_lit(const window_t* win) {
    render();   /* the hover only exists once the caption is repainted */
    /* Sampled mid-caption: near the left edge the rounded border's own
     * antialiasing tints the pixel, which would make every probe differ
     * from it. */
    uint32_t plain = win->surface->pixels[2 * win->surface->pitch + win->w / 2];
    for (int i = 0; i < 3; i++) {
        int cx = win->w - (3 - i) * WM_CAPTION_BTN_W + 4;
        if (cx < 0 || cx >= win->w) continue;
        if (win->surface->pixels[2 * win->surface->pitch + cx] != plain) return 1;
    }
    return 0;
}

static void key(uint16_t code, uint8_t ascii, uint8_t mods, uint8_t pressed) {
    key_event_t ev;
    ev.code = code;
    ev.ascii = ascii;
    ev.mods = mods;
    ev.pressed = pressed;
    handle_key(&ev);
}

static const char* shots_dir;

static void shot(const char* name) {
    if (!shots_dir) return;
    char path[256];
    snprintf(path, sizeof(path), "%s/%s.ppm", shots_dir, name);
    FILE* f = fopen(path, "wb");
    if (!f) return;

    render();
    fprintf(f, "P6\n%d %d\n255\n", screen_w, screen_h);
    for (int y = 0; y < screen_h; y++) {
        for (int x = 0; x < screen_w; x++) {
            uint32_t c = screen->pixels[y * screen->pitch + x];
            unsigned char rgb[3] = { GFX_RED(c), GFX_GREEN(c), GFX_BLUE(c) };
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
}

static void boot_shell(void) {
    screen_w = SCREEN_W;
    screen_h = SCREEN_H;
    screen = gfx_surface_create(screen_w, screen_h);
    wallpaper = gfx_surface_create(screen_w, screen_h);
    gfx_set_target(screen);
    render_wallpaper();

    cursor_x = prev_cursor_x = screen_w / 2;
    cursor_y = prev_cursor_y = screen_h / 2;

    wm_init(screen_w, screen_h, 0, TASKBAR_H);
    wm_set_sysmenu_handler(show_system_menu);
    desk_icons_layout();
    taskbar_layout();
}

/* --- the tests ---------------------------------------------------------- */

/* The taskbar is the only reserved edge, so the work area is the whole
 * screen above it - there is no menu bar to leave room for. */
static void test_work_area(void) {
    gfx_rect_t work;
    wm_work_area(&work);
    CHECK(work.x == 0 && work.y == 0, "work area starts at %d,%d, expected 0,0",
          work.x, work.y);
    CHECK(work.w == SCREEN_W && work.h == SCREEN_H - TASKBAR_H,
          "work area is %dx%d, expected %dx%d", work.w, work.h,
          SCREEN_W, SCREEN_H - TASKBAR_H);
}

/* Resizing must nail down every edge the press didn't grab. This is the
 * one that a screenshot cannot check and that regresses silently. */
static void test_resize_anchors(window_t* win) {
    wm_move(win, 300, 200);
    wm_resize(win, 600, 400);
    wm_focus(win);

    press(win->x + 2, win->y + 200);        /* 2px inside the left edge */
    drag(200, win->y + 200);
    release(200, win->y + 200);
    /* The grabbed edge keeps its 2px grip on the pointer, so a pointer at
     * 200 leaves the edge at 198 - and the right edge must not have
     * moved at all. */
    CHECK(win->x == 198, "left-edge resize put x at %d, expected 198", win->x);
    CHECK(win->x + win->w == 900,
          "left-edge resize moved the right edge to %d, expected 900",
          win->x + win->w);

    int right = win->x + win->w, bottom = win->y + win->h;
    press(win->x + 2, win->y + 2);          /* the top-left corner */
    drag(win->x + 60, win->y + 40);
    release(win->x + 60, win->y + 40);
    CHECK(win->x + win->w == right,
          "corner resize moved the right edge to %d, expected %d",
          win->x + win->w, right);
    CHECK(win->y + win->h == bottom,
          "corner resize moved the bottom edge to %d, expected %d",
          win->y + win->h, bottom);

    /* A resize can't drive a window below its minimum or above the work
     * area, however far the pointer goes. */
    press(win->x + win->w - 2, win->y + win->h - 2);
    drag(0, 0);
    release(0, 0);
    CHECK(win->w >= win->min_w && win->h >= win->min_h,
          "resize shrank the window to %dx%d, below its %dx%d minimum",
          win->w, win->h, win->min_w, win->min_h);
}

/* Aero Snap: the preview has to appear before the button comes up, and
 * the frame it previewed has to be the frame the window gets. */
static void test_snap(window_t* win) {
    wm_move(win, 300, 200);
    wm_resize(win, 600, 400);
    int fw = win->w, fh = win->h;

    /* Hover the close button, then grab the title bar: the hover has to
     * let go, or the button stays lit red for the whole drag. */
    move(win->x + win->w - 23, win->y + 16);
    gfx_rect_t preview;
    press(win->x + 200, win->y + 10);
    drag(1, 300);
    CHECK(!caption_button_lit(win),
          "a caption button stayed lit after the drag started");
    CHECK(wm_snap_preview(&preview), "no snap preview at the left edge");
    CHECK(preview.x == 0 && preview.y == 0 &&
          preview.w == SCREEN_W / 2 && preview.h == SCREEN_H - TASKBAR_H,
          "snap preview is %d,%d %dx%d, expected 0,0 %dx%d",
          preview.x, preview.y, preview.w, preview.h,
          SCREEN_W / 2, SCREEN_H - TASKBAR_H);
    shot("snap-preview");

    release(1, 300);
    CHECK(!wm_snap_preview(&preview), "the snap preview outlived the drag");
    CHECK(win->snapped == WM_SNAP_LEFT, "window did not snap left (state %d)",
          win->snapped);
    CHECK(win->x == 0 && win->w == SCREEN_W / 2 && win->h == SCREEN_H - TASKBAR_H,
          "snapped frame is %d,%d %dx%d", win->x, win->y, win->w, win->h);

    /* Dragging a snapped window away hands back its floating size. */
    press(win->x + 100, win->y + 10);
    drag(500, 300);
    CHECK(win->w == fw && win->h == fh,
          "dragging off the snap gave back %dx%d, expected %dx%d",
          win->w, win->h, fw, fh);
    release(500, 300);
    CHECK(win->snapped == WM_SNAP_NONE, "still snapped after being dragged off");

    /* The top edge maximizes rather than snapping to a half. */
    press(win->x + 100, win->y + 10);
    drag(600, 0);
    CHECK(wm_snap_preview(&preview) && preview.h == SCREEN_H - TASKBAR_H &&
          preview.w == SCREEN_W,
          "the top edge did not preview a maximize");
    release(600, 0);
    CHECK(win->maximized, "the top edge did not maximize");
    wm_restore(win);
}

/* Maximize fills the work area exactly and restore returns to the frame
 * the window had before it, not to an intermediate snap. */
static void test_maximize_restore(window_t* win) {
    wm_move(win, 320, 180);
    wm_resize(win, 620, 380);
    int fx = win->x, fy = win->y, fw = win->w, fh = win->h;

    wm_maximize(win);
    CHECK(win->x == 0 && win->y == 0 && win->w == SCREEN_W &&
          win->h == SCREEN_H - TASKBAR_H,
          "maximized frame is %d,%d %dx%d", win->x, win->y, win->w, win->h);

    /* Snap out of the maximize, then restore: the floating frame has to
     * survive the round trip. */
    wm_snap(win, WM_SNAP_RIGHT);
    CHECK(!win->maximized, "still flagged maximized after snapping");
    wm_restore(win);
    CHECK(win->x == fx && win->y == fy && win->w == fw && win->h == fh,
          "restored to %d,%d %dx%d, expected %d,%d %dx%d",
          win->x, win->y, win->w, win->h, fx, fy, fw, fh);
}

/* The caption buttons, hit-tested from the right edge inward. */
static void test_caption_buttons(window_t* win) {
    wm_move(win, 300, 200);
    wm_resize(win, 600, 400);
    wm_focus(win);

    int cy = win->y + WM_TITLEBAR_H / 2;
    int min_x = win->x + win->w - 3 * WM_CAPTION_BTN_W + WM_CAPTION_BTN_W / 2;
    int max_x = min_x + WM_CAPTION_BTN_W;
    int close_x = max_x + WM_CAPTION_BTN_W;

    /* Hovering a caption button has to mark the caption for repaint -
     * that repaint is the only thing that makes the hover visible, so if
     * it doesn't happen the close button never turns red. */
    move(win->x + 200, win->y + 200);       /* pointer in the client area */
    win->needs_paint = 0;
    move(close_x, cy);
    CHECK(win->needs_paint, "hovering the close button did not mark it for repaint");
    win->needs_paint = 0;
    move(win->x + 200, win->y + 200);
    CHECK(win->needs_paint, "leaving the close button did not clear its hover");

    click(min_x, cy);
    CHECK(win->minimized, "the minimize button did not minimize");
    wm_unminimize(win);

    click(max_x, cy);
    CHECK(win->maximized, "the maximize button did not maximize");
    /* The window just filled the work area, so the buttons moved with it -
     * the close button is now at the top-right of the *screen*. */
    click(win->x + win->w - WM_CAPTION_BTN_W / 2, win->y + WM_TITLEBAR_H / 2);
    CHECK(!win->open, "the close button did not close the window");
}

/* Windows shortcuts: Alt+F4, Win+arrows, Alt+Tab, Win to open Start. */
static void test_keyboard(void) {
    window_t* a = wm_open(&app_terminal, "one", 500, 340, 0);
    window_t* b = wm_open(&app_files, "two", 500, 340, 0);
    wm_focus(b);

    key(KEY_LEFT, 0, KEY_MOD_SUPER, 1);
    CHECK(b->snapped == WM_SNAP_LEFT, "Win+Left did not snap left");
    key(KEY_UP, 0, KEY_MOD_SUPER, 1);
    CHECK(b->maximized, "Win+Up did not maximize");
    key(KEY_DOWN, 0, KEY_MOD_SUPER, 1);
    CHECK(!b->maximized, "Win+Down did not restore");
    key(KEY_DOWN, 0, KEY_MOD_SUPER, 1);
    CHECK(b->minimized, "a second Win+Down did not minimize");
    wm_unminimize(b);

    /* Alt+Tab holds a switcher open until Alt comes up, and lands on the
     * previously-focused window. */
    key(KEY_TAB, '\t', KEY_MOD_ALT, 1);
    CHECK(switcher_open, "Alt+Tab did not open the switcher");
    CHECK(switcher_index == 1, "Alt+Tab started on entry %d, expected 1",
          switcher_index);
    shot("switcher");
    key(KEY_LALT, 0, 0, 0);      /* Alt released */
    CHECK(!switcher_open, "the switcher outlived the Alt key");
    CHECK(wm_focused() == a, "Alt+Tab focused the wrong window");

    /* Tapping the Windows key on its own toggles Start; using it as a
     * modifier must not. */
    key(KEY_SUPER, 0, KEY_MOD_SUPER, 1);
    key(KEY_SUPER, 0, 0, 0);
    CHECK(start_open, "tapping the Windows key did not open Start");
    key(KEY_ESCAPE, 27, 0, 1);
    CHECK(!start_open, "Escape did not close Start");

    key(KEY_SUPER, 0, KEY_MOD_SUPER, 1);
    key('d', 'd', KEY_MOD_SUPER, 1);
    key(KEY_SUPER, 0, 0, 0);
    CHECK(!start_open, "Win+D also opened the Start menu");

    key(KEY_F4, 0, KEY_MOD_ALT, 1);
    CHECK(!wm_focused() || wm_focused() != a || !a->open,
          "Alt+F4 did not close the focused window");
    if (a->open) wm_close(a);
    if (b->open) wm_close(b);
}

/* Show desktop puts back exactly what it hid, and nothing else. */
static void test_show_desktop(void) {
    window_t* a = wm_open(&app_terminal, "one", 400, 300, 0);
    window_t* b = wm_open(&app_files, "two", 400, 300, 0);
    wm_minimize(a);

    wm_toggle_show_desktop();
    CHECK(b->minimized, "show desktop left a window on screen");
    wm_toggle_show_desktop();
    CHECK(!b->minimized, "show desktop did not put its window back");
    CHECK(a->minimized, "show desktop restored a window it never hid");

    wm_close(a);
    wm_close(b);
}

/* Every open window gets a taskbar button, and clicking the focused one
 * minimizes it. */
static void test_taskbar(void) {
    window_t* a = wm_open(&app_terminal, "one", 400, 300, 0);
    window_t* b = wm_open(&app_files, "two", 400, 300, 0);
    wm_focus(b);
    taskbar_layout();

    int open_windows = 0;
    for (int i = 0; i < wm_count(); i++) {
        window_t* w = wm_at(i);
        if (w && w->open) open_windows++;
    }
    int buttons = 0, index = -1;
    for (int i = 0; i < task_count; i++) {
        if (task_items[i].kind != TB_WINDOW) continue;
        buttons++;
        if (task_items[i].win == b) index = i;
    }
    CHECK(buttons == open_windows,
          "taskbar shows %d window buttons for %d open windows",
          buttons, open_windows);
    CHECK(index >= 0, "the focused window has no taskbar button");

    if (index >= 0) {
        int bx = task_items[index].x + task_items[index].w / 2;
        int by = screen_h - TASKBAR_H / 2;
        click(bx, by);
        CHECK(b->minimized, "clicking the focused window's button did not minimize it");
        click(bx, by);
        CHECK(!b->minimized, "clicking it again did not restore it");
    }

    wm_close(a);
    wm_close(b);
}

/* The Start menu searches apps and the initrd, and its entries are
 * clickable where they are drawn. */
static void test_start_menu(void) {
    start_show();
    CHECK(start_entry_count > 0, "the Start menu opened empty");

    int tiles = 0;
    for (int i = 0; i < start_entry_count; i++) {
        if (start_entries[i].tile) tiles++;
    }
    CHECK(tiles == 6, "the pinned grid has %d tiles, expected 6", tiles);
    shot("start");

    /* Typing filters the list. "exe" is a substring of hellowin.exe and
     * of nothing else - "File Explorer" has an "e", a space and an "E",
     * which is not the same thing. */
    key('e', 'e', 0, 1);
    key('x', 'x', 0, 1);
    key('e', 'e', 0, 1);
    int found_exe = 0, found_explorer = 0, tiles_left = 0;
    for (int i = 0; i < start_entry_count; i++) {
        if (!kstrcmp(start_entries[i].name, "hellowin.exe")) found_exe = 1;
        if (!kstrcmp(start_entries[i].name, "File Explorer")) found_explorer = 1;
        if (start_entries[i].tile) tiles_left++;
    }
    CHECK(found_exe, "searching for \"exe\" did not match hellowin.exe");
    CHECK(!found_explorer, "searching for \"exe\" matched File Explorer");
    CHECK(tiles_left == 0, "the pinned grid survived a search");
    shot("start-search");

    /* Backspace all the way out and type something that does match an
     * app, to prove the filter runs over both lists. */
    for (int i = 0; i < 3; i++) key(KEY_BACKSPACE, '\b', 0, 1);
    key('t', 't', 0, 1);
    key('a', 'a', 0, 1);
    key('s', 's', 0, 1);
    key('k', 'k', 0, 1);
    int found_taskmgr = 0;
    for (int i = 0; i < start_entry_count; i++) {
        if (!kstrcmp(start_entries[i].name, "Task Manager")) found_taskmgr = 1;
    }
    CHECK(found_taskmgr, "searching for \"task\" did not match Task Manager");

    /* Every entry's rectangle has to hit-test back to itself. */
    for (int i = 0; i < start_entry_count; i++) {
        gfx_rect_t r = start_entries[i].r;
        int hit = start_entry_at(r.x + r.w / 2, r.y + r.h / 2);
        CHECK(hit == i, "entry %d hit-tests as %d", i, hit);
    }
    start_hide();
}

int main(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--shots") && i + 1 < argc) shots_dir = argv[++i];
    }

    strcpy(host_files[0].name, "readme.txt");
    strcpy(host_files[1].name, "hello.txt");
    strcpy(host_files[2].name, "hellowin.exe");
    for (int i = 0; i < 3; i++) host_files[i].length = 1234;
    vfs_root = &host_files[0];

    boot_shell();

    window_t* files = wm_open(&app_files, "File Explorer " UI_EMDASH " This Novaris",
                              700, 430, 0);
    window_t* term = wm_open(&app_terminal, "novaris " UI_EMDASH " shell", 760, 430, 0);
    CHECK(files && term, "the first two windows did not open");
    if (!files || !term) return 1;

    move(term->x + term->w - 23, term->y + 16);   /* hovering close */
    shot("desktop");

    test_work_area();
    test_resize_anchors(term);
    test_snap(term);
    test_maximize_restore(term);
    test_caption_buttons(term);   /* closes `term` */
    test_keyboard();
    test_show_desktop();
    test_taskbar();
    test_start_menu();

    printf("%d checks, %d failure%s\n", checks, failures,
           failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
