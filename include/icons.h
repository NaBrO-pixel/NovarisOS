#ifndef ICONS_H
#define ICONS_H

#include "gfx.h"
#include "wm.h"

/* icons - every pictogram the shell draws, in one place.
 *
 * There are no image files anywhere in Novaris: the initrd is a handful
 * of text files and a couple of .exe binaries, and adding a bitmap format
 * plus a decoder to draw a folder would be a lot of machinery for a
 * folder. So every icon is drawn from primitives at whatever size the
 * caller asks for, which has a second payoff - the same routine serves a
 * 16px title-bar icon, a 24px taskbar button and a 40px desktop icon
 * without three sets of artwork drifting apart.
 *
 * Coordinates inside each routine are fractions of `size`, so nothing
 * here assumes a particular pixel grid.
 */

/* The four-pane mark on the Start button. */
void icon_window_logo(int x, int y, int size, uint32_t color);

/* An app's icon. Dispatches on the app_t pointer; unknown apps get a
 * generic tile rather than nothing, so a window always has an icon. */
void icon_app(const app_t* app, int x, int y, int size);

/* Shell icons that don't belong to an app. */
void icon_computer(int x, int y, int size);
void icon_recycle_bin(int x, int y, int size);
void icon_document(int x, int y, int size);
void icon_folder(int x, int y, int size);

/* Small monochrome glyphs: taskbar tray, search fields, menus. `size` is
 * the glyph's box; they are drawn centered in it. */
void icon_search(int x, int y, int size, uint32_t color);
void icon_power(int x, int y, int size, uint32_t color);
void icon_network(int x, int y, int size, uint32_t color);
void icon_volume(int x, int y, int size, uint32_t color);
void icon_chevron_up(int x, int y, int size, uint32_t color);

#endif
