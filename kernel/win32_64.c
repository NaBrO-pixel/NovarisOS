/* win32_64.c - the Win32 functions a 64-bit PE can call, so far.
 *
 * Three of them. That is not a stand-in for a plan: the point of this
 * milestone is the loader and the calling convention, and the honest way
 * to widen it later is Milestone 45's tools/pe_imports.py, which says
 * exactly which functions a given .exe needs. */

#include "win32_64.h"
#include "kstring.h"
#include "serial64.h"

/* Handles are opaque to the program, so these can be anything that is
 * not NULL and not INVALID_HANDLE_VALUE. */
#define HANDLE_STDOUT 1
#define HANDLE_STDERR 2

#define STD_OUTPUT_HANDLE ((uint32_t)-11)
#define STD_ERROR_HANDLE  ((uint32_t)-12)

enum {
    FN_GETSTDHANDLE = WIN32_64_BASE,
    FN_WRITEFILE,
    FN_GETLASTERROR,
    FN_SETLASTERROR,
    /* ExitProcess is WIN32_64_EXIT rather than the next number here: the
     * syscall stub has to recognise it without calling into C, because
     * it is the one that does not return. */
};

static const struct {
    const char* name;
    int         number;
} exports[] = {
    { "GetStdHandle",  FN_GETSTDHANDLE },
    { "WriteFile",     FN_WRITEFILE    },
    { "GetLastError",  FN_GETLASTERROR },
    { "SetLastError",  FN_SETLASTERROR },
    { "ExitProcess",   WIN32_64_EXIT   },
};

static uint64_t bytes_written;
static uint32_t last_error;

uint64_t win32_64_bytes_written(void) { return bytes_written; }

int win32_64_resolve(const char* dll, const char* function) {
    /* Only kernel32 so far. The DLL name is compared case-insensitively
     * because import tables contain "KERNEL32.dll", "kernel32.DLL" and
     * everything between. */
    if (kstricmp(dll, "KERNEL32.dll") != 0 &&
        kstricmp(dll, "kernel32.dll") != 0)
        return -1;

    for (unsigned i = 0; i < sizeof(exports) / sizeof(exports[0]); i++)
        if (kstrcmp(function, exports[i].name) == 0)
            return exports[i].number;

    return -1;
}

uint64_t win32_64_call(uint64_t number, uint64_t a1, uint64_t a2,
                       uint64_t a3, uint64_t a4) {
    switch (number) {
    case FN_GETSTDHANDLE:
        if ((uint32_t)a1 == STD_OUTPUT_HANDLE) return HANDLE_STDOUT;
        if ((uint32_t)a1 == STD_ERROR_HANDLE)  return HANDLE_STDERR;
        return 0;   /* NULL: "no such handle" */

    /* WriteFile(hFile, lpBuffer, nBytes, lpWritten, lpOverlapped).
     * Returns non-zero on success, unlike write(2) which returns the
     * count - the count goes through the fourth argument instead. */
    case FN_WRITEFILE: {
        const char* buf = (const char*)a2;
        uint32_t n = (uint32_t)a3;
        uint32_t* written = (uint32_t*)a4;

        if (a1 != HANDLE_STDOUT && a1 != HANDLE_STDERR) {
            last_error = 6;             /* ERROR_INVALID_HANDLE */
            return 0;
        }
        for (uint32_t i = 0; i < n; i++) serial64_putc(buf[i]);
        bytes_written += n;
        /* lpNumberOfBytesWritten may legitimately be NULL only when
         * lpOverlapped is not; nothing here does overlapped I/O, so a
         * NULL is simply not written to. */
        if (written) *written = n;
        return 1;
    }

    case FN_GETLASTERROR:
        return last_error;

    case FN_SETLASTERROR:
        last_error = (uint32_t)a1;
        return 0;

    default:
        return 0;
    }
}
