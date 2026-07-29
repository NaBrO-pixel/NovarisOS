#ifndef UIKIT_H
#define UIKIT_H

#include "gfx.h"

/* uikit - the handful of widgets the built-in apps share, so About,
 * File Explorer, Task Manager and the alert panel don't each grow their
 * slightly different button. Deliberately tiny: this is a system UI, not
 * a toolkit anyone else builds against. */

#define UI_TEXT        GFX_RGB(0x1A, 0x1A, 0x1A)
#define UI_TEXT_DIM    GFX_RGB(0x60, 0x60, 0x66)
#define UI_TEXT_FAINT  GFX_RGB(0x8A, 0x8A, 0x90)
#define UI_ACCENT      GFX_RGB(0x00, 0x78, 0xD4)   /* the Windows system blue */
#define UI_ACCENT_HOT  GFX_RGB(0x1A, 0x88, 0xDD)   /* accent, hovered */
#define UI_SEPARATOR   GFX_RGB(0xE3, 0xE3, 0xE6)
#define UI_PANEL       GFX_RGB(0xF3, 0xF3, 0xF3)   /* sidebar / toolbar fill */
#define UI_CARD        GFX_RGB(0xFF, 0xFF, 0xFF)
#define UI_ROW_ALT     GFX_RGB(0xF7, 0xF7, 0xF9)

/* Windows draws a button as a flat, lightly rounded rectangle with a
 * hairline border - no shadow, no gradient. This is the corner radius
 * every control here uses so they read as one family. */
#define UI_RADIUS      4

enum { UI_BTN_NORMAL, UI_BTN_HOVER, UI_BTN_PRESSED, UI_BTN_DEFAULT };

void ui_button(int x, int y, int w, int h, const char* label, int state);
int  ui_hit(int x, int y, int w, int h, int mx, int my);

/* A right-aligned label / left-aligned value pair, the layout the About
 * window and Task Manager both list system facts in. */
void ui_key_value(int x, int y, int label_w, const char* key, const char* value);

void ui_separator(int x, int y, int w);
void ui_progress(int x, int y, int w, int h, int percent, uint32_t color);

/* Number formatting, since there's no snprintf down here. Each writes a
 * NUL-terminated string and returns `out`. */
char* ui_fmt_uint(char* out, uint32_t value);
/* Thousands-separated, e.g. "1,048,576". */
char* ui_fmt_grouped(char* out, uint32_t value);
/* Human-readable size, e.g. "12 KB", "1.2 MB". */
char* ui_fmt_bytes(char* out, uint32_t bytes);
/* "3:04:11" from a count of seconds. */
char* ui_fmt_duration(char* out, uint32_t seconds);

#endif
