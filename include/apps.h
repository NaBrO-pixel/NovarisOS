#ifndef APPS_H
#define APPS_H

#include "wm.h"

/* The apps that ship with the desktop. Each is an app_t vtable plus an
 * "open me" function the taskbar, the Start menu and the desktop icons
 * all call. Every one of these opens a single window and reuses it if
 * it's already open, which is how clicking a taskbar button twice
 * behaves. */

/* Terminal - the window the kernel shell lives in. Its buffer is a
 * singleton: there is one shell, with one line being edited, so a second
 * Terminal window would be two views of one conversation. */
extern const app_t app_terminal;
window_t* terminal_open(void);
/* Installs the console sink and seeds the buffer with the boot log. Call
 * once, before the first frame. */
void terminal_app_init(void);
/* Types a command into the shell as if the user had. */
void terminal_run(const char* command);

/* File Explorer - browses the initrd. */
extern const app_t app_files;
window_t* files_open(void);

/* Task Manager - live memory, uptime and system counters. */
extern const app_t app_monitor;
window_t* monitor_open(void);

/* About Novaris. */
extern const app_t app_about;
window_t* about_open(void);

/* A read-only text viewer, opened by Files. */
extern const app_t app_textview;
window_t* textview_open(const char* filename);

/* Opens whatever `path` names, the way double-clicking it would: a text
 * file in the viewer, a .exe under Wine when Wine is installed and under
 * the built-in Win32 layer when it is not, anything else executable
 * through the shell's `run`. One entry point so that the File Explorer,
 * the desktop and the Start menu cannot disagree. */
void app_open_path(const char* path);

/* A modal-looking alert panel. Not actually modal - it's a window like
 * any other, it just looks the part. */
extern const app_t app_alert;
window_t* alert_open(const char* heading, const char* message);

#endif
