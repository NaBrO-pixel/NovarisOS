/* uikit.c - shared widgets for the built-in apps. See include/uikit.h. */

#include "uikit.h"
#include "kstring.h"

void ui_button(int x, int y, int w, int h, const char* label, int state) {
    uint32_t fill, text, border;
    switch (state) {
        case UI_BTN_DEFAULT:
            fill = UI_ACCENT;
            text = GFX_RGB(0xFF, 0xFF, 0xFF);
            border = GFX_ARGB(0x33, 0, 0, 0);
            break;
        case UI_BTN_PRESSED:
            fill = GFX_RGB(0xF0, 0xF0, 0xF0);
            text = UI_TEXT_DIM;
            border = GFX_RGB(0xD8, 0xD8, 0xDB);
            break;
        case UI_BTN_HOVER:
            fill = GFX_RGB(0xF7, 0xF7, 0xF7);
            text = UI_TEXT;
            border = GFX_RGB(0xD0, 0xD0, 0xD4);
            break;
        default:
            fill = GFX_RGB(0xFD, 0xFD, 0xFD);
            text = UI_TEXT;
            border = GFX_RGB(0xDA, 0xDA, 0xDE);
            break;
    }
    /* Flat, lightly rounded, hairline border - and one darker line along
     * the bottom edge. That last line is the whole trick: it is all that
     * remains of the bevel a Windows button used to have, and without it
     * a flat rectangle reads as a text field rather than a button. */
    gfx_round_rect(x, y, w, h, UI_RADIUS, fill);
    gfx_round_frame(x, y, w, h, UI_RADIUS, 1, border);
    if (state != UI_BTN_PRESSED) {
        gfx_fill(x + UI_RADIUS, y + h - 1, w - UI_RADIUS * 2, 1,
                 state == UI_BTN_DEFAULT ? GFX_ARGB(0x44, 0, 0, 0)
                                         : GFX_RGB(0xC2, 0xC2, 0xC6));
    }
    gfx_text_center(&uifont_regular, x, y + (h - uifont_regular.line_height) / 2,
                    w, label, text);
}

int ui_hit(int x, int y, int w, int h, int mx, int my) {
    return mx >= x && my >= y && mx < x + w && my < y + h;
}

void ui_key_value(int x, int y, int label_w, const char* key, const char* value) {
    gfx_text_right(&uifont_bold, x, y, label_w, key, UI_TEXT_DIM);
    gfx_text(&uifont_regular, x + label_w + 10, y, value, UI_TEXT);
}

void ui_separator(int x, int y, int w) {
    gfx_fill(x, y, w, 1, UI_SEPARATOR);
}

void ui_progress(int x, int y, int w, int h, int percent, uint32_t color) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    gfx_round_rect(x, y, w, h, h / 2, GFX_RGB(0xE3, 0xE3, 0xE8));
    int filled = w * percent / 100;
    if (filled > 0) {
        if (filled < h) filled = h;   /* a sliver still reads as a pill */
        gfx_round_rect(x, y, filled, h, h / 2, color);
    }
}

char* ui_fmt_uint(char* out, uint32_t value) {
    ku32_to_dec(value, out);
    return out;
}

char* ui_fmt_grouped(char* out, uint32_t value) {
    char digits[12];
    uint32_t n = ku32_to_dec(value, digits);
    uint32_t o = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (i > 0 && ((n - i) % 3) == 0) out[o++] = ',';
        out[o++] = digits[i];
    }
    out[o] = '\0';
    return out;
}

char* ui_fmt_bytes(char* out, uint32_t bytes) {
    char num[12];
    if (bytes < 1024) {
        ku32_to_dec(bytes, num);
        kstrcpy(out, num);
        kstrcat(out, " bytes");
    } else if (bytes < 1024 * 1024) {
        ku32_to_dec(bytes / 1024, num);
        kstrcpy(out, num);
        kstrcat(out, " KB");
    } else {
        /* One decimal place, worked out by hand - there's no float here. */
        ku32_to_dec(bytes / (1024 * 1024), num);
        kstrcpy(out, num);
        uint32_t tenths = (bytes % (1024 * 1024)) * 10 / (1024 * 1024);
        if (tenths) {
            kstrcat(out, ".");
            ku32_to_dec(tenths, num);
            kstrcat(out, num);
        }
        kstrcat(out, " MB");
    }
    return out;
}

char* ui_fmt_duration(char* out, uint32_t seconds) {
    char num[12];
    uint32_t h = seconds / 3600, m = (seconds / 60) % 60, s = seconds % 60;
    out[0] = '\0';
    if (h) {
        ku32_to_dec(h, num);
        kstrcat(out, num);
        kstrcat(out, ":");
        if (m < 10) kstrcat(out, "0");
    }
    ku32_to_dec(m, num);
    kstrcat(out, num);
    kstrcat(out, ":");
    if (s < 10) kstrcat(out, "0");
    ku32_to_dec(s, num);
    kstrcat(out, num);
    return out;
}
