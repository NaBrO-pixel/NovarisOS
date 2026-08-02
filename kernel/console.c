#include "console.h"
#include "framebuffer.h"
#include "font8x16.h"
#include "vga_text.h"
#include "serial.h"

#define CHAR_W 8
#define CHAR_H 16

/* Desktop chrome layout, in pixels. */
#define TITLEBAR_H       24
#define TASKBAR_H        26
#define WIN_MARGIN       14
#define WIN_TITLEBAR_H   20
#define WIN_BORDER       2
#define WIN_PADDING      6

/* Standard 16-color VGA palette, mapped to real RGB. These are the
 * well-known CGA/EGA/VGA hardware palette values - a fixed table of
 * numbers, not anyone's creative work. */
static const uint8_t palette_rgb[16][3] = {
    {0x00, 0x00, 0x00}, {0x00, 0x00, 0xAA}, {0x00, 0xAA, 0x00}, {0x00, 0xAA, 0xAA},
    {0xAA, 0x00, 0x00}, {0xAA, 0x00, 0xAA}, {0xAA, 0x55, 0x00}, {0xAA, 0xAA, 0xAA},
    {0x55, 0x55, 0x55}, {0x55, 0x55, 0xFF}, {0x55, 0xFF, 0x55}, {0x55, 0xFF, 0xFF},
    {0xFF, 0x55, 0x55}, {0xFF, 0x55, 0xFF}, {0xFF, 0xFF, 0x55}, {0xFF, 0xFF, 0xFF},
};

static int has_framebuffer = 0;

/* Desktop chrome colors (fixed, not part of the 16-color text palette). */
static uint32_t col_desktop, col_titlebar, col_taskbar;
static uint32_t col_win_titlebar, col_win_bg, col_win_border;

/* The text area's pixel origin and size, inset within the "window". */
static uint32_t area_x, area_y, area_w_px, area_h_px;
static uint32_t cols, rows; /* text area size, in characters */

static uint32_t terminal_row, terminal_column;
static uint8_t terminal_color;

/* Where output goes once the desktop owns the screen (see console.h), and
 * a recording of everything written before that happened. 8K cells is
 * comfortably more than the boot log's ~3K characters. */
static console_sink_t sink;
#define BOOT_LOG_MAX 8192
static console_cell_t boot_log[BOOT_LOG_MAX];
static uint32_t boot_log_len;

static uint32_t palette_lookup(uint8_t index) {
    return fb_rgb(palette_rgb[index & 0xF][0], palette_rgb[index & 0xF][1],
                  palette_rgb[index & 0xF][2]);
}

/* Draws one glyph at a pixel position, filling its whole 8x16 cell (bg
 * color where the font bit is 0, fg where it's 1) so scrolling/overwrite
 * never leaves stale pixels behind. */
static void draw_glyph_px(uint32_t px, uint32_t py, char c, uint32_t fg, uint32_t bg) {
    const uint8_t* rows_bitmap;
    static const uint8_t blank[16] = {0};

    if (c >= FONT8X16_FIRST_CHAR && c <= FONT8X16_LAST_CHAR) {
        rows_bitmap = font8x16[(unsigned char)c - FONT8X16_FIRST_CHAR];
    } else {
        rows_bitmap = blank;
    }

    for (int y = 0; y < CHAR_H; y++) {
        uint8_t bits = rows_bitmap[y];
        for (int x = 0; x < CHAR_W; x++) {
            int bit = (bits >> (7 - x)) & 1;
            fb_put_pixel(px + x, py + y, bit ? fg : bg);
        }
    }
}

static void draw_string_px(uint32_t px, uint32_t py, const char* s, uint32_t fg, uint32_t bg) {
    uint32_t x = px;
    while (*s) {
        draw_glyph_px(x, py, *s, fg, bg);
        x += CHAR_W;
        s++;
    }
}

static void draw_chrome(void) {
    uint32_t w = fb_width();
    uint32_t h = fb_height();

    fb_fill_rect(0, 0, w, h, col_desktop);

    fb_fill_rect(0, 0, w, TITLEBAR_H, col_titlebar);
    draw_string_px(10, 4, "Novaris OS", fb_rgb(255, 255, 255), col_titlebar);

    fb_fill_rect(0, h - TASKBAR_H, w, TASKBAR_H, col_taskbar);
    draw_string_px(10, h - TASKBAR_H + 5, "Milestone 7: framebuffer + mouse",
                   fb_rgb(230, 230, 230), col_taskbar);

    uint32_t win_x = WIN_MARGIN;
    uint32_t win_y = TITLEBAR_H + WIN_MARGIN;
    uint32_t win_w = w - 2 * WIN_MARGIN;
    uint32_t win_h = h - TITLEBAR_H - TASKBAR_H - 2 * WIN_MARGIN;

    /* Border, drawn as a slightly larger rect behind the window body. */
    fb_fill_rect(win_x - WIN_BORDER, win_y - WIN_BORDER,
                 win_w + 2 * WIN_BORDER, win_h + 2 * WIN_BORDER, col_win_border);

    fb_fill_rect(win_x, win_y, win_w, WIN_TITLEBAR_H, col_win_titlebar);
    draw_string_px(win_x + 8, win_y + 2, "Novaris Shell", fb_rgb(255, 255, 255),
                   col_win_titlebar);

    fb_fill_rect(win_x, win_y + WIN_TITLEBAR_H, win_w, win_h - WIN_TITLEBAR_H, col_win_bg);

    area_x = win_x + WIN_PADDING;
    area_y = win_y + WIN_TITLEBAR_H + WIN_PADDING;
    area_w_px = win_w - 2 * WIN_PADDING;
    area_h_px = win_h - WIN_TITLEBAR_H - 2 * WIN_PADDING;

    cols = area_w_px / CHAR_W;
    rows = area_h_px / CHAR_H;
}

int console_use_framebuffer(uint32_t addr, uint32_t pitch, uint32_t width,
                             uint32_t height, uint8_t bpp) {
    if (!fb_init(addr, pitch, width, height, bpp)) return 0;
    if (fb_width() < 400 || fb_height() < 300) return 0; /* too small to be useful */

    col_desktop = fb_rgb(0x1c, 0x3f, 0x5e);
    col_titlebar = fb_rgb(0x0f, 0x24, 0x3e);
    col_taskbar = fb_rgb(0x0f, 0x24, 0x3e);
    col_win_titlebar = fb_rgb(0x2e, 0x5c, 0x8a);
    col_win_bg = fb_rgb(0x08, 0x08, 0x08);
    col_win_border = fb_rgb(0x6a, 0x9c, 0xc7);

    has_framebuffer = 1;
    return 1;
}

int console_has_framebuffer(void) {
    return has_framebuffer;
}

void terminal_initialize(void) {
    if (has_framebuffer) {
        draw_chrome();
        terminal_row = 0;
        terminal_column = 0;
        terminal_color = (uint8_t)(VGA_COLOR_LIGHT_GREY | (VGA_COLOR_BLACK << 4));
    } else {
        vga_text_initialize();
    }
}

void terminal_setcolor(uint8_t color) {
    terminal_color = color;
    if (!has_framebuffer) vga_text_setcolor(color);
}

uint8_t terminal_getcolor(void) { return terminal_color; }

void console_set_sink(console_sink_t new_sink) { sink = new_sink; }

void console_clear(void) {
    if (sink) {
        /* A form feed is the terminal convention for "clear the screen",
         * and nothing else in the kernel's output ever emits one. */
        sink('\f', terminal_color);
        return;
    }
    terminal_initialize();
}

const console_cell_t* console_boot_log(uint32_t* out_len) {
    if (out_len) *out_len = boot_log_len;
    return boot_log;
}

uint32_t console_vga_rgb(uint8_t index) {
    return palette_lookup(index);
}

static void fb_scroll_one_line(void) {
    uint32_t bg = palette_lookup((terminal_color >> 4) & 0xF);
    fb_scroll_up(area_x, area_y, area_w_px, area_h_px, CHAR_H, bg);
    terminal_row = rows - 1;
}

static void putchar_plain(char c) {
    /* Mirror every character out COM1 before drawing it, whichever
     * backend is live. This is the transcript tools/qemu_test.py asserts
     * against - see serial.h. */
    serial_putchar(c);

    if (sink) {
        sink(c, terminal_color);
        return;
    }
    if (boot_log_len < BOOT_LOG_MAX) {
        boot_log[boot_log_len].c = c;
        boot_log[boot_log_len].color = terminal_color;
        boot_log_len++;
    }

    if (!has_framebuffer) {
        vga_text_putchar(c);
        return;
    }

    uint32_t fg = palette_lookup(terminal_color & 0xF);
    uint32_t bg = palette_lookup((terminal_color >> 4) & 0xF);

    if (c == '\n') {
        terminal_column = 0;
        if (++terminal_row == rows) fb_scroll_one_line();
        return;
    }

    draw_glyph_px(area_x + terminal_column * CHAR_W, area_y + terminal_row * CHAR_H,
                  c, fg, bg);
    if (++terminal_column == cols) {
        terminal_column = 0;
        if (++terminal_row == rows) fb_scroll_one_line();
    }
}

/* --- a minimal ANSI escape reader (Milestone 30) -------------------------
 *
 * Novaris's console is a framebuffer and a font, not a terminal, and for
 * twenty-nine milestones nothing needed it to be one: the kernel and its
 * programs write plain text.
 *
 * Wine's conhost does not. It draws a Windows console by positioning a
 * VT cursor - every space is "erase to end of line, cursor forward one",
 * every character is bracketed by hide-cursor/show-cursor - so the first
 * Windows program to run under Wine here produced perfectly correct text
 * with an escape sequence between each letter of it.
 *
 * So: the handful of sequences a console actually uses. Not a terminal
 * emulator, and deliberately not - anything unrecognised is swallowed
 * rather than printed, because printing it is what the problem looked
 * like. Applied before the serial mirror, so the transcript the test
 * harness reads is the text and not the drawing instructions. */
#define ANSI_MAX_PARAMS 8

static enum { ANSI_NONE = 0, ANSI_ESC, ANSI_CSI } ansi_state = ANSI_NONE;
static int  ansi_params[ANSI_MAX_PARAMS];
static int  ansi_nparams;
static int  ansi_private;

static void ansi_erase_to_eol(void) {
    if (!has_framebuffer) return;
    uint32_t bg = palette_lookup((terminal_color >> 4) & 0xF);
    for (uint32_t c = terminal_column; c < cols; c++) {
        draw_glyph_px(area_x + c * CHAR_W, area_y + terminal_row * CHAR_H,
                      ' ', bg, bg);
    }
}

/* SGR, as far as a 16-colour palette can honour it. The bright bit is
 * kept separately from the colour so that "bold" then "red" gives bright
 * red, which is what every terminal does and what a program setting them
 * in either order expects. */
static void ansi_sgr(void) {
    static const uint8_t vga_of_ansi[8] = { 0, 4, 2, 6, 1, 5, 3, 7 };
    static int bright = 0;
    for (int i = 0; i < ansi_nparams; i++) {
        int p = ansi_params[i];
        if (p == 0) {
            bright = 0;
            terminal_color = VGA_COLOR_LIGHT_GREY;
        } else if (p == 1) {
            bright = 8;
            terminal_color = (uint8_t)((terminal_color & 0xF0) |
                                       ((terminal_color & 0x07) | 8));
        } else if (p >= 30 && p <= 37) {
            terminal_color = (uint8_t)((terminal_color & 0xF0) |
                                       vga_of_ansi[p - 30] | bright);
        } else if (p >= 40 && p <= 47) {
            terminal_color = (uint8_t)((terminal_color & 0x0F) |
                                       (vga_of_ansi[p - 40] << 4));
        } else if (p >= 90 && p <= 97) {
            terminal_color = (uint8_t)((terminal_color & 0xF0) |
                                       vga_of_ansi[p - 90] | 8);
        }
    }
}

/* Acts on a complete CSI sequence. `final` is the letter that ended it. */
static void ansi_dispatch(char final) {
    int n = ansi_nparams > 0 ? ansi_params[0] : 0;

    /* Private sequences (ESC [ ? ...) are the cursor-visibility ones and
     * a few others; none of them mean anything to a console that has no
     * hardware cursor to hide. */
    if (ansi_private) return;

    switch (final) {
        case 'C':   /* cursor forward - which is how conhost writes a space */
            if (n < 1) n = 1;
            for (int i = 0; i < n; i++) putchar_plain(' ');
            break;
        case 'D':   /* cursor back */
            if (n < 1) n = 1;
            for (int i = 0; i < n && terminal_column > 0; i++) terminal_column--;
            break;
        case 'K':   /* erase to end of line */
            ansi_erase_to_eol();
            break;
        case 'H': case 'f': {   /* cursor position, 1-based */
            uint32_t r = (ansi_nparams > 0 && ansi_params[0] > 0)
                             ? (uint32_t)ansi_params[0] - 1 : 0;
            uint32_t c = (ansi_nparams > 1 && ansi_params[1] > 0)
                             ? (uint32_t)ansi_params[1] - 1 : 0;
            if (r < rows) terminal_row = r;
            if (c < cols) terminal_column = c;
            break;
        }
        case 'J':   /* erase display: only "the whole of it" is used */
            if (n == 2) {
                console_clear();
            } else {
                ansi_erase_to_eol();
            }
            break;
        case 'm':
            ansi_sgr();
            break;
        default:
            break;   /* recognised as a sequence, and ignored */
    }
}

void terminal_putchar(char c) {
    switch (ansi_state) {
        case ANSI_NONE:
            if (c == 0x1B) { ansi_state = ANSI_ESC; return; }
            break;
        case ANSI_ESC:
            if (c == '[') {
                ansi_state = ANSI_CSI;
                ansi_nparams = 0;
                ansi_private = 0;
                ansi_params[0] = 0;
                return;
            }
            /* Not a CSI. Two-character escapes are not used by anything
             * here, so the sequence ends and the character is dropped. */
            ansi_state = ANSI_NONE;
            return;
        case ANSI_CSI:
            if (c == '?') { ansi_private = 1; return; }
            if (c >= '0' && c <= '9') {
                if (ansi_nparams == 0) ansi_nparams = 1;
                int* p = &ansi_params[ansi_nparams - 1];
                *p = *p * 10 + (c - '0');
                return;
            }
            if (c == ';') {
                if (ansi_nparams < ANSI_MAX_PARAMS) {
                    ansi_params[ansi_nparams++] = 0;
                }
                return;
            }
            ansi_state = ANSI_NONE;
            ansi_dispatch(c);
            return;
    }
    putchar_plain(c);
}

void terminal_backspace(void) {
    serial_putchar('\b');

    if (sink) {
        sink('\b', terminal_color);
        return;
    }

    if (!has_framebuffer) {
        vga_text_backspace();
        return;
    }

    if (terminal_column == 0) {
        if (terminal_row == 0) return;
        terminal_row--;
        terminal_column = cols - 1;
    } else {
        terminal_column--;
    }

    uint32_t bg = palette_lookup((terminal_color >> 4) & 0xF);
    draw_glyph_px(area_x + terminal_column * CHAR_W, area_y + terminal_row * CHAR_H,
                  ' ', bg, bg);
}

void terminal_write(const char* data, size_t size) {
    for (size_t i = 0; i < size; i++) terminal_putchar(data[i]);
}

void terminal_writestring(const char* data) {
    /* Always goes through terminal_write -> terminal_putchar, including
     * in VGA text mode: the old text-mode fast path called
     * vga_text_writestring() directly, which would now skip the serial
     * mirror added to terminal_putchar. */
    size_t len = 0;
    while (data[len]) len++;
    terminal_write(data, len);
}

void terminal_writestring_color(const char* data, uint8_t color) {
    uint8_t saved = terminal_color;
    terminal_setcolor(color);
    terminal_writestring(data);
    terminal_setcolor(saved);
}
