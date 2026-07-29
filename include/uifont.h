#ifndef UIFONT_H
#define UIFONT_H

#include <stdint.h>

/* The desktop's proportional, antialiased typeface (see
 * tools/gen_uifont.py, which generates kernel/uifont.c).
 *
 * font8x16 stays exactly where it was - it is the terminal font, and a
 * fixed 8x16 cell is what a character grid wants. This is the other half:
 * variable-width glyphs with real bearings and 8-bit coverage, for window
 * titles, menus, Dock labels and everything else the compositor draws. */

typedef struct {
    uint16_t offset;  /* start of this glyph's coverage map in `pixels` */
    uint8_t  w, h;    /* coverage map dimensions (0x0 for whitespace) */
    int8_t   left;    /* map's left edge, relative to the pen position */
    int8_t   top;     /* map's top edge, above the baseline (may be negative) */
    uint8_t  advance; /* how far the pen moves after drawing */
} uifont_glyph_t;

typedef struct {
    uint8_t first, last;   /* inclusive character range with glyph entries */
    uint8_t ascent;        /* baseline to the top of the line box */
    uint8_t descent;       /* baseline to the bottom of the line box */
    uint8_t line_height;   /* ascent + descent */
    const uifont_glyph_t* glyphs;
    const uint8_t* pixels;
} uifont_t;

extern const uifont_t uifont_small;   /* 11px - Dock labels, menu bar extras */
extern const uifont_t uifont_regular; /* 13px - body text, menu items */
extern const uifont_t uifont_bold;    /* 13px bold - window titles, app name */
extern const uifont_t uifont_title;   /* 22px bold - headings */

/* Control codes 1-15 carry UI symbols in every face, so a plain C string
 * can embed one: "\x01Q" renders as the command-key glyph followed by Q.
 * 8/9/10/13 are left blank on purpose - they are \b \t \n \r. */
#define UI_CMD      "\x01"
#define UI_OPTION   "\x02"
#define UI_SHIFT    "\x03"
#define UI_CONTROL  "\x04"
#define UI_CHECK    "\x05"
#define UI_SUBMENU  "\x06"
#define UI_RUNDOT   "\x07"
#define UI_BULLET   "\x0b"
#define UI_ELLIPSIS "\x0c"
#define UI_ARROW    "\x0e"
#define UI_HAMBURGER "\x0f"
#define UI_EMDASH   "\x10"
#define UI_DELETE   "\x11"

#endif
