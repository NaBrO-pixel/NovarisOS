/* app_terminal.c - the Terminal window: the kernel shell, in a window.
 *
 * The shell itself didn't change much (kernel/shell.c still parses and
 * runs the commands). What changed is where its output goes: console.c
 * now hands each character to a sink, and this file is that sink. It
 * keeps a character grid with scrollback, draws it in the 8x16 terminal
 * font, and feeds keystrokes back the other way.
 *
 * The one subtle piece is live output. `run hellowin.exe` executes
 * synchronously inside a keystroke's dispatch, so without help the screen
 * would freeze until the program finished and then show everything at
 * once. Instead, the sink asks the desktop to composite and present a
 * frame every few ticks, so output appears as it's produced - a program
 * printing a hundred lines looks like a program printing a hundred lines.
 */

#include "apps.h"
#include "gfx.h"
#include "console.h"
#include "keyboard.h"
#include "shell.h"
#include "desktop.h"
#include "kstring.h"
#include "pit.h"

#define TERM_MAX_COLS   200
#define TERM_SCROLLBACK 400

#define PAD_X 12
#define PAD_Y 10
#define LINE_H GFX_MONO_H

#define TERM_BG        GFX_RGB(0x1B, 0x1B, 0x1D)
#define TERM_BG_HEADER GFX_RGB(0x24, 0x24, 0x27)
#define TERM_CURSOR    GFX_RGB(0xC8, 0xF0, 0xB0)

typedef struct {
    char c;
    uint8_t color;
} cell_t;

/* Rows are addressed by an ever-increasing absolute number and stored in
 * a ring, so scrolling is an index change rather than a 150KB memmove per
 * line of output. */
static cell_t grid[TERM_SCROLLBACK][TERM_MAX_COLS];
static uint32_t row_len[TERM_SCROLLBACK];

static int cur_row;        /* absolute row the cursor is on */
static int cur_col;
static int view_top;       /* absolute row drawn at the top of the window */
static int cols = 80, rows = 24;
static int cursor_visible = 1;
static uint32_t cursor_tick;

static window_t* term_window;
static uint32_t last_pump_tick;
static int in_pump;

/* A modern-terminal reading of the 16 VGA colors: the same palette the
 * kernel has always written in, chosen to be legible on a dark
 * background instead of a white one. */
static const uint32_t palette[16] = {
    GFX_RGB(0x30, 0x30, 0x34), GFX_RGB(0x4A, 0x6F, 0xE3),
    GFX_RGB(0x3E, 0xA7, 0x6A), GFX_RGB(0x3C, 0xA5, 0xA5),
    GFX_RGB(0xD0, 0x46, 0x3B), GFX_RGB(0xB3, 0x6A, 0xC0),
    GFX_RGB(0xC0, 0x8A, 0x3E), GFX_RGB(0xD2, 0xD2, 0xD6),
    GFX_RGB(0x80, 0x80, 0x88), GFX_RGB(0x7A, 0xA2, 0xF7),
    GFX_RGB(0x67, 0xD4, 0x8B), GFX_RGB(0x6E, 0xD6, 0xD6),
    GFX_RGB(0xFF, 0x6E, 0x63), GFX_RGB(0xD1, 0x8A, 0xE0),
    GFX_RGB(0xE5, 0xC0, 0x7B), GFX_RGB(0xFF, 0xFF, 0xFF),
};

static cell_t* row_cells(int absolute) {
    return grid[((absolute % TERM_SCROLLBACK) + TERM_SCROLLBACK) % TERM_SCROLLBACK];
}

static void clear_row(int absolute) {
    cell_t* r = row_cells(absolute);
    for (int i = 0; i < TERM_MAX_COLS; i++) {
        r[i].c = ' ';
        r[i].color = 0x07;
    }
    row_len[((absolute % TERM_SCROLLBACK) + TERM_SCROLLBACK) % TERM_SCROLLBACK] = 0;
}

static void scroll_into_view(void) {
    if (cur_row - view_top >= rows) view_top = cur_row - rows + 1;
    if (view_top > cur_row) view_top = cur_row;
    if (view_top < 0) view_top = 0;
}

static void newline(void) {
    cur_row++;
    cur_col = 0;
    clear_row(cur_row);
    scroll_into_view();
}

static void put_char(char c, uint8_t color) {
    if (c == '\n') { newline(); return; }
    if (c == '\r') { cur_col = 0; return; }
    if (c == '\f') {
        /* Form feed: the `clear` command. Leave the scrollback intact but
         * start the visible area on a fresh row. */
        for (int i = 0; i < rows; i++) { cur_row++; clear_row(cur_row); }
        cur_row -= rows - 1;
        cur_col = 0;
        view_top = cur_row;
        return;
    }
    if (c == '\b') {
        if (cur_col > 0) {
            cur_col--;
            row_cells(cur_row)[cur_col].c = ' ';
        }
        return;
    }
    if (c == '\t') {
        int stop = (cur_col + 8) & ~7;
        while (cur_col < stop && cur_col < cols - 1) put_char(' ', color);
        return;
    }
    if (c < 32 || c > 126) return;

    if (cur_col >= cols) newline();
    cell_t* r = row_cells(cur_row);
    r[cur_col].c = c;
    r[cur_col].color = color;
    cur_col++;
    uint32_t idx = ((cur_row % TERM_SCROLLBACK) + TERM_SCROLLBACK) % TERM_SCROLLBACK;
    if ((uint32_t)cur_col > row_len[idx]) row_len[idx] = (uint32_t)cur_col;
}

/* console.c calls this for every character the kernel prints. */
static void terminal_sink(char c, uint8_t color) {
    put_char(c, color);
    scroll_into_view();
    if (!term_window) return;

    wm_invalidate(term_window);

    /* Push a frame out periodically while a long-running command prints,
     * so its output streams instead of arriving all at once when it
     * finishes. The guard matters: compositing repaints this window,
     * which must not recurse back into here. */
    if (c == '\n' && !in_pump) {
        uint32_t now = pit_get_ticks();
        if (now - last_pump_tick >= 4) {
            last_pump_tick = now;
            in_pump = 1;
            desktop_pump_output();
            in_pump = 0;
        }
    }
}

void terminal_app_init(void) {
    for (int i = 0; i < TERM_SCROLLBACK; i++) clear_row(i);
    cur_row = 0;
    cur_col = 0;
    view_top = 0;

    /* Replay the boot log so the Terminal opens showing what the kernel
     * did on the way up, exactly as it would have scrolled past. */
    uint32_t len = 0;
    const console_cell_t* log = console_boot_log(&len);
    for (uint32_t i = 0; i < len; i++) put_char(log[i].c, log[i].color);

    console_set_sink(terminal_sink);
}

static void recompute_geometry(window_t* win) {
    int new_cols = (wm_client_w(win) - PAD_X * 2) / GFX_MONO_W;
    int new_rows = (wm_client_h(win) - PAD_Y * 2) / LINE_H;
    if (new_cols < 20) new_cols = 20;
    if (new_cols > TERM_MAX_COLS) new_cols = TERM_MAX_COLS;
    if (new_rows < 4) new_rows = 4;
    cols = new_cols;
    rows = new_rows;
    scroll_into_view();
}

static void terminal_paint(window_t* win) {
    recompute_geometry(win);
    int cw = wm_client_w(win), ch = wm_client_h(win);
    gfx_fill(0, 0, cw, ch, TERM_BG);

    for (int line = 0; line < rows; line++) {
        int absolute = view_top + line;
        if (absolute < 0 || absolute > cur_row) continue;
        if (cur_row - absolute >= TERM_SCROLLBACK) continue;

        const cell_t* r = row_cells(absolute);
        uint32_t used = row_len[((absolute % TERM_SCROLLBACK) + TERM_SCROLLBACK)
                                % TERM_SCROLLBACK];
        int y = PAD_Y + line * LINE_H;
        for (uint32_t col = 0; col < used && (int)col < cols; col++) {
            if (r[col].c == ' ') continue;
            gfx_mono_char(PAD_X + (int)col * GFX_MONO_W, y, r[col].c,
                          palette[r[col].color & 0x0F]);
        }
    }

    /* The cursor sits on the shell's line; it's hidden while scrolled
     * back, the same way a terminal hides it when you're reading history. */
    int cursor_line = cur_row - view_top;
    if (cursor_visible && cursor_line >= 0 && cursor_line < rows) {
        int x = PAD_X + cur_col * GFX_MONO_W;
        int y = PAD_Y + cursor_line * LINE_H;
        if (win == wm_focused()) {
            gfx_fill(x, y, GFX_MONO_W, LINE_H - 1, TERM_CURSOR);
            const cell_t* r = row_cells(cur_row);
            if (cur_col < TERM_MAX_COLS && r[cur_col].c != ' ') {
                gfx_mono_char(x, y, r[cur_col].c, TERM_BG);
            }
        } else {
            gfx_frame(x, y, GFX_MONO_W, LINE_H - 1, TERM_CURSOR);
        }
    }

    /* A scrollback hint, so it's obvious you're not looking at the live
     * end of the output. */
    if (view_top + rows <= cur_row) {
        const char* label = "scrolled back " UI_ARROW " press End";
        int tw = gfx_text_width(&uifont_small, label);
        gfx_round_rect(cw - tw - 26, ch - 28, tw + 16, 20, 10,
                       GFX_ARGB(0xCC, 0x3A, 0x3A, 0x40));
        gfx_text(&uifont_small, cw - tw - 18, ch - 25, label,
                 GFX_RGB(0xE0, 0xE0, 0xE6));
    }
}

static void terminal_key(window_t* win, const key_event_t* ev) {
    if (!ev->pressed) return;

    switch (ev->code) {
        case KEY_PAGE_UP:
            view_top -= rows - 1;
            if (view_top < cur_row - TERM_SCROLLBACK + rows) {
                view_top = cur_row - TERM_SCROLLBACK + rows;
            }
            if (view_top < 0) view_top = 0;
            wm_invalidate(win);
            return;
        case KEY_PAGE_DOWN:
            view_top += rows - 1;
            scroll_into_view();
            wm_invalidate(win);
            return;
        case KEY_END:
            view_top = cur_row - rows + 1;
            if (view_top < 0) view_top = 0;
            wm_invalidate(win);
            return;
        default: break;
    }

    if (!ev->ascii) return;
    if (ev->mods & KEY_MOD_COMMAND) return;  /* menu shortcuts, not input */

    view_top = cur_row - rows + 1;   /* typing jumps back to the live end */
    if (view_top < 0) view_top = 0;
    cursor_visible = 1;
    shell_feed_char((char)ev->ascii);
    wm_invalidate(win);
}

static void terminal_mouse(window_t* win, int x, int y, int kind, int buttons,
                           int wheel) {
    (void)x; (void)y; (void)buttons;
    if (kind != WM_MOUSE_WHEEL) return;
    view_top -= wheel * 3;
    int oldest = cur_row - TERM_SCROLLBACK + rows;
    if (view_top < oldest) view_top = oldest;
    if (view_top < 0) view_top = 0;
    if (view_top > cur_row) view_top = cur_row;
    wm_invalidate(win);
}

static void terminal_tick(window_t* win) {
    uint32_t now = pit_get_ticks();
    if (now - cursor_tick < 50) return;   /* ~2 Hz, like a real terminal */
    cursor_tick = now;
    cursor_visible = !cursor_visible;
    /* Only the cursor cell changed, so damage only the cursor cell. */
    win->needs_paint = 1;
    wm_invalidate_rect(win, PAD_X + cur_col * GFX_MONO_W,
                       PAD_Y + (cur_row - view_top) * LINE_H,
                       GFX_MONO_W + 1, LINE_H);
}

static void terminal_closed(window_t* win) {
    if (term_window == win) term_window = 0;
}

const app_t app_terminal = {
    "Terminal", terminal_paint, terminal_key, terminal_mouse, terminal_tick,
    terminal_closed,
};

window_t* terminal_open(void) {
    if (term_window) {
        wm_focus(term_window);
        return term_window;
    }
    term_window = wm_open(&app_terminal, "novaris " UI_EMDASH " shell", 760, 470, 0);
    if (term_window) {
        term_window->bg = TERM_BG;
        term_window->min_w = 340;
        term_window->min_h = 180;
    }
    return term_window;
}

void terminal_run(const char* command) {
    window_t* win = terminal_open();
    if (!win) return;
    wm_focus(win);
    shell_run_line(command);
    wm_invalidate(win);
}
