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
    if (has_framebuffer) {
        terminal_color = color;
    } else {
        vga_text_setcolor(color);
    }
}

static void fb_scroll_one_line(void) {
    uint32_t bg = palette_lookup((terminal_color >> 4) & 0xF);
    fb_scroll_up(area_x, area_y, area_w_px, area_h_px, CHAR_H, bg);
    terminal_row = rows - 1;
}

void terminal_putchar(char c) {
    /* Mirror every character out COM1 before drawing it, whichever
     * backend is live. This is the transcript tools/qemu_test.py asserts
     * against - see serial.h. */
    serial_putchar(c);

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

void terminal_backspace(void) {
    serial_putchar('\b');

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
