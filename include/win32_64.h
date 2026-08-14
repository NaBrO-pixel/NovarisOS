#ifndef WIN32_64_H
#define WIN32_64_H

#include <stdint.h>

/* The Win32 functions a 64-bit PE can import, and the syscall numbers
 * they are reached by.
 *
 * These are Novaris's own numbers, not Windows'. Windows has no stable
 * syscall interface - a program calls kernel32, kernel32 calls ntdll,
 * and ntdll's numbers change between builds - so there is nothing to be
 * compatible with at this level, and the compatibility that matters is
 * at the *API* boundary: the names, their arguments, and the calling
 * convention. That is what pe64.c's thunks bridge.
 *
 * Kept well clear of Linux's range so a stray Linux call cannot be
 * mistaken for a Win32 one. */
#define WIN32_64_BASE  0x2000
#define WIN32_64_EXIT  0x2FFF   /* ExitProcess: leaves ring 3          */

/* Returns the syscall number for an imported name, or -1 if this kernel
 * does not have it. Matching is case-insensitive on the DLL name, since
 * import tables spell them every possible way, and exact on the function
 * name, since Win32 exports are case-sensitive. */
int win32_64_resolve(const char* dll, const char* function);

/* Dispatched from syscall64.c for numbers in the Win32 range. The
 * arguments have already been shuffled out of the Windows convention by
 * the thunk. */
uint64_t win32_64_call(uint64_t number, uint64_t a1, uint64_t a2,
                       uint64_t a3, uint64_t a4);

uint64_t win32_64_bytes_written(void);

#endif
