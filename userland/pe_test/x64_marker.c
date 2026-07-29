/* x64_marker.c - a 64-bit PE, built only to be refused.
 *
 * Novaris is a 32-bit kernel, so a PE32+ image can never run here. What
 * matters is that `run` says *that* - "64-bit (PE32+) binary" - instead
 * of "malformed PE image" or, worse, mapping it and jumping into
 * instructions the CPU will decode as something else entirely. This is
 * the smallest binary that makes the point: built with
 * x86_64-w64-mingw32-gcc -nostdlib, so it's a couple of KB rather than
 * the quarter-megabyte a CRT-linked one would be.
 *
 * It never executes a single instruction on Novaris, which is the test. */

int mainCRTStartup(void) {
    return 0;
}
