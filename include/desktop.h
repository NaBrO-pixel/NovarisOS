#ifndef DESKTOP_H
#define DESKTOP_H

#include <stdint.h>

/* desktop - the shell you actually see: wallpaper, desktop icons, the
 * taskbar, the Start menu, and the event loop that ties the window
 * manager to the keyboard and mouse drivers.
 *
 * This is the layer that replaced shell_run() as the kernel's final
 * destination. The shell is still there - it's an app now (see
 * kernel/app_terminal.c), the way a shell is on any windowing OS.
 */

/* Brings up the compositor and opens the default windows, then runs the
 * event loop forever. Requires a framebuffer console; kernel_main falls
 * back to shell_run() when there isn't one. Never returns. */
void desktop_start(void);

/* Composites and presents one frame without touching the input queues.
 *
 * This exists for one reason: a shell command runs to completion inside
 * the keystroke that started it, so the only way its output can appear
 * while it's still running is for the code printing that output to push a
 * frame itself. The Terminal's console sink calls this every few ticks.
 * Not reentrant - the caller guards against being re-entered from its own
 * repaint. */
void desktop_pump_output(void);

/* One whole turn of the desktop: input drained and routed, housekeeping,
 * and a composite of whatever is damaged. Returns 1 if it drew.
 *
 * The event loop is this called forever. It is public because Milestone
 * 36 gave a ring-3 process a window (kernel/wmdev.c), and such a process
 * runs *inside* a shell command - which runs inside the loop. Its window
 * would never be drawn while it lived, so its ioctls pump the desktop
 * themselves: a frame per damage, and input per poll. Self-guarded
 * against reentry, so calling it from a repaint is harmless. */
int desktop_pump(void);

/* The strip along the bottom of the screen the taskbar owns. Everything
 * above it is the work area: what a maximized window fills. */
int desktop_taskbar_height(void);

#endif
