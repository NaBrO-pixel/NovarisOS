#ifndef SHELL_H
#define SHELL_H

/* The kernel shell: a small set of built-in commands over a line editor.
 *
 * It used to own the input loop - read a key, echo it, run the line. That
 * doesn't work once the shell lives inside a window, because the desktop
 * owns the event loop and hands out keystrokes to whichever window has
 * focus. So the loop is turned inside out: the shell exposes "here is a
 * character" and the caller decides where characters come from.
 */

/* Prints the banner and the first prompt. */
void shell_init(void);

/* Feeds one character of input: prints it, handles backspace, and runs
 * the line on '\n'. */
void shell_feed_char(char c);

/* Runs a whole command as if it had been typed, echo and all. Used by the
 * Files window when you open a program. */
void shell_run_line(const char* line);

/* The blocking loop, still used when there's no framebuffer to put a
 * desktop on: reads the keyboard directly and never returns. */
void shell_run(void);

#endif
