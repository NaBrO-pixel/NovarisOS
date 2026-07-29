/* app_files.c - Files (a browser for the initrd) and the text viewer it
 * opens.
 *
 * The initrd is a flat archive with no directories, so the sidebar's
 * "folders" are filters over one list rather than a tree - which is
 * honest about what the filesystem is, and still gives the window the
 * shape a file browser should have. Double-clicking does the obvious
 * thing per file type: text opens in a viewer, an executable is handed to
 * the shell exactly as if you had typed `run` in the Terminal.
 */

#include "apps.h"
#include "uikit.h"
#include "gfx.h"
#include "vfs.h"
#include "kheap.h"
#include "kstring.h"

#define SIDEBAR_W   158
#define TOOLBAR_H   44
#define HEADER_H    24
#define ROW_H       28
#define MAX_FILES   64

enum { FILTER_ALL, FILTER_PROGRAMS, FILTER_DOCUMENTS, FILTER_COUNT };

static const char* filter_names[FILTER_COUNT] = {
    "All Files", "Programs", "Documents"
};

typedef struct {
    int filter;
    int selected;
    int scroll;         /* first visible row */
    int hover_row;
    int count;
    vfs_node_t* nodes[MAX_FILES];
} files_state_t;

static files_state_t files_state;

/* --- file classification ----------------------------------------------- */

enum { KIND_TEXT, KIND_EXE, KIND_ELF, KIND_BIN, KIND_OTHER };

static int kind_of(const char* name) {
    if (kstr_ends_with_ci(name, ".txt")) return KIND_TEXT;
    if (kstr_ends_with_ci(name, ".exe")) return KIND_EXE;
    if (kstr_ends_with_ci(name, ".elf")) return KIND_ELF;
    if (kstr_ends_with_ci(name, ".bin")) return KIND_BIN;
    return KIND_OTHER;
}

static const char* kind_label(int kind) {
    switch (kind) {
        case KIND_TEXT: return "Plain Text";
        case KIND_EXE:  return "Windows Application";
        case KIND_ELF:  return "ELF Executable";
        case KIND_BIN:  return "Flat Binary";
        default:        return "Document";
    }
}

static int passes_filter(int filter, int kind) {
    if (filter == FILTER_ALL) return 1;
    if (filter == FILTER_PROGRAMS) {
        return kind == KIND_EXE || kind == KIND_ELF || kind == KIND_BIN;
    }
    return kind == KIND_TEXT || kind == KIND_OTHER;
}

static void rebuild(files_state_t* st) {
    st->count = 0;
    if (!vfs_root) return;
    uint32_t idx = 0;
    vfs_node_t* node;
    while ((node = vfs_readdir(vfs_root, idx)) && st->count < MAX_FILES) {
        if (passes_filter(st->filter, kind_of(node->name))) {
            st->nodes[st->count++] = node;
        }
        idx++;
    }
    if (st->selected >= st->count) st->selected = st->count - 1;
    if (st->scroll > st->count - 1) st->scroll = 0;
}

/* --- icons -------------------------------------------------------------- */

/* Drawn rather than stored: at 20x20 a procedural icon costs a dozen
 * lines and no bytes of image data. */
static void draw_icon(int x, int y, int kind) {
    if (kind == KIND_TEXT || kind == KIND_OTHER) {
        gfx_round_rect(x + 2, y, 16, 20, 2, GFX_RGB(0xFF, 0xFF, 0xFF));
        gfx_round_frame(x + 2, y, 16, 20, 2, 1, GFX_RGB(0xB8, 0xB8, 0xC0));
        /* the folded corner */
        gfx_fill(x + 12, y, 6, 6, GFX_RGB(0xE4, 0xE4, 0xEA));
        for (int i = 0; i < 4; i++) {
            gfx_fill(x + 5, y + 7 + i * 3, kind == KIND_TEXT ? 10 : 7, 1,
                     GFX_RGB(0xB0, 0xB0, 0xB8));
        }
        return;
    }
    uint32_t top, bottom;
    if (kind == KIND_EXE) {
        top = GFX_RGB(0x4C, 0x8D, 0xFF);
        bottom = GFX_RGB(0x1E, 0x5B, 0xD6);
    } else if (kind == KIND_ELF) {
        top = GFX_RGB(0x6E, 0xC7, 0x7B);
        bottom = GFX_RGB(0x2F, 0x8C, 0x48);
    } else {
        top = GFX_RGB(0x9B, 0x9B, 0xA6);
        bottom = GFX_RGB(0x6A, 0x6A, 0x76);
    }
    gfx_round_rect_gradient(x + 1, y + 1, 18, 18, 5, top, bottom);
    gfx_round_frame(x + 1, y + 1, 18, 18, 5, 1, GFX_ARGB(0x30, 0, 0, 0));
    gfx_text(&uifont_small, x + 4, y + 4,
             kind == KIND_EXE ? "EXE" : (kind == KIND_ELF ? "ELF" : "BIN"),
             GFX_RGB(0xFF, 0xFF, 0xFF));
}

/* --- painting ----------------------------------------------------------- */

static void files_paint(window_t* win) {
    files_state_t* st = &files_state;
    int cw = wm_client_w(win), ch = wm_client_h(win);
    rebuild(st);

    /* Sidebar. */
    gfx_fill(0, 0, SIDEBAR_W, ch, UI_PANEL);
    gfx_text(&uifont_small, 16, 14, "FAVORITES", UI_TEXT_FAINT);
    for (int i = 0; i < FILTER_COUNT; i++) {
        int y = 36 + i * 28;
        if (i == st->filter) {
            gfx_round_rect(8, y - 4, SIDEBAR_W - 16, 26, 6,
                           GFX_ARGB(0x22, 0x00, 0x7A, 0xFF));
        }
        /* a small folder mark */
        gfx_round_rect(18, y + 2, 14, 11, 2,
                       i == st->filter ? UI_ACCENT : GFX_RGB(0x9A, 0x9A, 0xA4));
        gfx_fill(18, y, 6, 3, i == st->filter ? UI_ACCENT
                                              : GFX_RGB(0x9A, 0x9A, 0xA4));
        gfx_text(&uifont_regular, 42, y, filter_names[i],
                 i == st->filter ? UI_ACCENT : UI_TEXT);
    }
    gfx_fill(SIDEBAR_W - 1, 0, 1, ch, UI_SEPARATOR);

    /* Toolbar. */
    int lx = SIDEBAR_W;
    int lw = cw - SIDEBAR_W;
    gfx_fill(lx, 0, lw, TOOLBAR_H, GFX_RGB(0xFA, 0xFA, 0xFC));
    gfx_text(&uifont_bold, lx + 16, 13, filter_names[st->filter], UI_TEXT);

    char count[32], num[12];
    ui_fmt_uint(num, (uint32_t)st->count);
    kstrcpy(count, num);
    kstrcat(count, st->count == 1 ? " item" : " items");
    gfx_text_right(&uifont_small, lx, 15, lw - 16, count, UI_TEXT_FAINT);
    gfx_fill(lx, TOOLBAR_H - 1, lw, 1, UI_SEPARATOR);

    /* Column headers. */
    int hy = TOOLBAR_H;
    gfx_fill(lx, hy, lw, HEADER_H, GFX_RGB(0xF5, 0xF5, 0xF8));
    gfx_text(&uifont_small, lx + 42, hy + 5, "Name", UI_TEXT_DIM);
    gfx_text(&uifont_small, lx + lw - 250, hy + 5, "Size", UI_TEXT_DIM);
    gfx_text(&uifont_small, lx + lw - 170, hy + 5, "Kind", UI_TEXT_DIM);
    gfx_fill(lx, hy + HEADER_H - 1, lw, 1, UI_SEPARATOR);

    /* Rows. */
    int list_y = hy + HEADER_H;
    int visible = (ch - list_y) / ROW_H;
    gfx_save();
    if (gfx_clip(lx, list_y, lw, ch - list_y)) {
        for (int i = 0; i < visible; i++) {
            int index = st->scroll + i;
            if (index >= st->count) break;
            vfs_node_t* node = st->nodes[index];
            int y = list_y + i * ROW_H;
            int kind = kind_of(node->name);

            uint32_t text_color = UI_TEXT, dim = UI_TEXT_DIM;
            if (index == st->selected) {
                gfx_fill(lx, y, lw, ROW_H, UI_ACCENT);
                text_color = GFX_RGB(0xFF, 0xFF, 0xFF);
                dim = GFX_ARGB(0xCC, 0xFF, 0xFF, 0xFF);
            } else if (index == st->hover_row) {
                gfx_fill(lx, y, lw, ROW_H, GFX_RGB(0xEE, 0xEE, 0xF4));
            } else if (index & 1) {
                gfx_fill(lx, y, lw, ROW_H, UI_ROW_ALT);
            }

            draw_icon(lx + 14, y + 4, kind);
            gfx_text_ellipsized(&uifont_regular, lx + 42, y + 6, lw - 300,
                                node->name, text_color);

            char size[24];
            ui_fmt_bytes(size, node->length);
            gfx_text(&uifont_small, lx + lw - 250, y + 8, size, dim);
            gfx_text(&uifont_small, lx + lw - 170, y + 8, kind_label(kind), dim);
        }
        if (st->count == 0) {
            gfx_text_center(&uifont_regular, lx, list_y + 40, lw,
                            "No files", UI_TEXT_FAINT);
        }
    }
    gfx_restore();

    /* Status bar. */
    gfx_fill(lx, ch - 22, lw, 22, GFX_RGB(0xF5, 0xF5, 0xF8));
    gfx_fill(lx, ch - 22, lw, 1, UI_SEPARATOR);
    gfx_text(&uifont_small, lx + 12, ch - 19,
             "Double-click to open " UI_BULLET
             " programs run in the Terminal window", UI_TEXT_FAINT);
}

/* --- interaction --------------------------------------------------------- */

static int row_at(window_t* win, int x, int y) {
    if (x < SIDEBAR_W) return -1;
    int list_y = TOOLBAR_H + HEADER_H;
    if (y < list_y || y >= wm_client_h(win) - 22) return -1;
    int index = files_state.scroll + (y - list_y) / ROW_H;
    return index < files_state.count ? index : -1;
}

static int sidebar_at(int x, int y) {
    if (x >= SIDEBAR_W) return -1;
    for (int i = 0; i < FILTER_COUNT; i++) {
        if (ui_hit(8, 32 + i * 28, SIDEBAR_W - 16, 26, x, y)) return i;
    }
    return -1;
}

static void open_selected(window_t* win) {
    files_state_t* st = &files_state;
    if (st->selected < 0 || st->selected >= st->count) return;
    vfs_node_t* node = st->nodes[st->selected];

    int kind = kind_of(node->name);
    if (kind == KIND_TEXT || kind == KIND_OTHER) {
        textview_open(node->name);
        return;
    }
    char command[96];
    kstrcpy(command, "run ");
    kstrcat(command, node->name);
    terminal_run(command);
    (void)win;
}

static void files_mouse(window_t* win, int x, int y, int kind, int buttons,
                        int wheel) {
    (void)buttons;
    files_state_t* st = &files_state;

    if (kind == WM_MOUSE_WHEEL) {
        st->scroll -= wheel * 3;
        int visible = (wm_client_h(win) - TOOLBAR_H - HEADER_H - 22) / ROW_H;
        if (st->scroll > st->count - visible) st->scroll = st->count - visible;
        if (st->scroll < 0) st->scroll = 0;
        wm_invalidate(win);
        return;
    }

    if (kind == WM_MOUSE_MOVE) {
        int row = row_at(win, x, y);
        if (row != st->hover_row) {
            st->hover_row = row;
            wm_invalidate(win);
        }
        return;
    }

    if (kind == WM_MOUSE_DOWN || kind == WM_MOUSE_DOUBLE) {
        int side = sidebar_at(x, y);
        if (side >= 0) {
            st->filter = side;
            st->selected = -1;
            st->scroll = 0;
            wm_invalidate(win);
            return;
        }
        int row = row_at(win, x, y);
        if (row >= 0) {
            st->selected = row;
            wm_invalidate(win);
            if (kind == WM_MOUSE_DOUBLE) open_selected(win);
        }
    }
}

static void files_key(window_t* win, const key_event_t* ev) {
    if (!ev->pressed) return;
    files_state_t* st = &files_state;
    int visible = (wm_client_h(win) - TOOLBAR_H - HEADER_H - 22) / ROW_H;

    switch (ev->code) {
        case KEY_UP:   if (st->selected > 0) st->selected--; break;
        case KEY_DOWN: if (st->selected < st->count - 1) st->selected++; break;
        case KEY_ENTER: open_selected(win); return;
        default: return;
    }
    if (st->selected < st->scroll) st->scroll = st->selected;
    if (st->selected >= st->scroll + visible) st->scroll = st->selected - visible + 1;
    wm_invalidate(win);
}

const app_t app_files = {
    "Files", files_paint, files_key, files_mouse, 0, 0,
};

window_t* files_open(void) {
    window_t* existing = wm_find_by_app(&app_files);
    if (existing) {
        wm_focus(existing);
        return existing;
    }
    files_state.filter = FILTER_ALL;
    files_state.selected = -1;
    files_state.hover_row = -1;
    files_state.scroll = 0;
    window_t* win = wm_open(&app_files, "Novaris " UI_EMDASH " initrd", 700, 430, 0);
    if (win) win->min_w = 520;
    return win;
}

/* --- text viewer --------------------------------------------------------- */

typedef struct {
    char name[64];
    char* text;
    uint32_t length;
    int scroll;
} textview_state_t;

static void textview_paint(window_t* win) {
    textview_state_t* st = (textview_state_t*)win->data;
    int cw = wm_client_w(win), ch = wm_client_h(win);
    gfx_fill(0, 0, cw, ch, GFX_RGB(0xFF, 0xFF, 0xFF));

    int line = 0, col = 0;
    int y0 = 12 - st->scroll * GFX_MONO_H;
    int max_cols = (cw - 28) / GFX_MONO_W;

    for (uint32_t i = 0; i < st->length; i++) {
        char c = st->text[i];
        if (c == '\n' || col >= max_cols) {
            line++;
            col = 0;
            if (c == '\n') continue;
        }
        if (c == '\r' || c == '\t') { col += (c == '\t') ? 4 : 0; continue; }
        int y = y0 + line * GFX_MONO_H;
        if (y > ch) break;
        if (y >= -GFX_MONO_H) {
            gfx_mono_char(14 + col * GFX_MONO_W, y, c, GFX_RGB(0x22, 0x22, 0x26));
        }
        col++;
    }
}

static void textview_mouse(window_t* win, int x, int y, int kind, int buttons,
                           int wheel) {
    (void)x; (void)y; (void)buttons;
    if (kind != WM_MOUSE_WHEEL) return;
    textview_state_t* st = (textview_state_t*)win->data;
    st->scroll -= wheel * 3;
    if (st->scroll < 0) st->scroll = 0;
    wm_invalidate(win);
}

static void textview_closed(window_t* win) {
    textview_state_t* st = (textview_state_t*)win->data;
    if (!st) return;
    kfree(st->text);
    kfree(st);
}

const app_t app_textview = {
    "TextView", textview_paint, 0, textview_mouse, 0, textview_closed,
};

window_t* textview_open(const char* filename) {
    vfs_node_t* node = vfs_root ? vfs_finddir(vfs_root, filename) : 0;
    if (!node) {
        alert_open("File not found", filename);
        return 0;
    }

    textview_state_t* st = (textview_state_t*)kmalloc(sizeof(textview_state_t));
    if (!st) return 0;
    st->text = (char*)kmalloc(node->length + 1);
    if (!st->text) {
        kfree(st);
        alert_open("Out of memory", "There isn't enough free memory to open "
                                    "that file.");
        return 0;
    }
    st->length = vfs_read(node, 0, node->length, (uint8_t*)st->text);
    st->text[st->length] = '\0';
    st->scroll = 0;
    kstrlcpy(st->name, filename, sizeof(st->name));

    window_t* win = wm_open(&app_textview, st->name, 560, 400, st);
    if (!win) {
        kfree(st->text);
        kfree(st);
    }
    return win;
}
