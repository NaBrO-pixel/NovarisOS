#ifndef WIN32_INTERNAL_H
#define WIN32_INTERNAL_H

#include <stdint.h>
#include "win32.h"

/* Shared plumbing between kernel/win32.c (the core: thunks, dispatch,
 * process setup) and the files that implement the actual APIs
 * (win32_kernel32.c, win32_msvcrt.c, win32_user32.c). Not a public
 * interface - the shell and the loader only need include/win32.h. */

/* --- the module tables ------------------------------------------------- */

extern const win32_module_t win32_modules[];
extern const uint32_t win32_module_table_count;

/* --- console ----------------------------------------------------------- */

/* Everything an emulated program prints goes through here, so there's one
 * place that handles the console attribute set by SetConsoleTextAttribute
 * and one place that turns CRLF into the single newline the Novaris
 * console expects. */
void w32_write(const char* text, uint32_t length);
void w32_write_str(const char* text);
void w32_set_attribute(uint16_t attribute);
uint16_t w32_get_attribute(void);

/* Kernel-side diagnostics, printed in a distinct color so they can't be
 * confused with the program's own output. */
void w32_report(const char* text);
void w32_report_hex(const char* label, uint32_t value);
void w32_report_dec(const char* label, uint32_t value);

/* Reads one line from the keyboard with echo and backspace handling,
 * appending a newline. Returns the number of bytes written. */
uint32_t w32_read_line(char* buffer, uint32_t size);

/* --- errors ------------------------------------------------------------ */

void     w32_set_error(uint32_t code);
uint32_t w32_get_error(void);

#define ERROR_SUCCESS             0u
#define ERROR_FILE_NOT_FOUND      2u
#define ERROR_ACCESS_DENIED       5u
#define ERROR_INVALID_HANDLE      6u
#define ERROR_NOT_ENOUGH_MEMORY   8u
#define ERROR_INVALID_PARAMETER  87u
#define ERROR_CALL_NOT_IMPLEMENTED 120u
#define ERROR_NO_MORE_FILES      18u
#define ERROR_INVALID_FUNCTION    1u

/* --- user-visible memory ----------------------------------------------- */

/* Allocator over the Win32 heap arena (WIN32_HEAP_BASE). Everything an
 * emulated program can see a pointer to - HeapAlloc, malloc, VirtualAlloc,
 * the environment block, FILE structs - comes from here rather than from
 * kmalloc, because kmalloc hands back kernel addresses at 0xD0000000 that
 * ring 3 is not allowed to touch. */
uint32_t w32_mem_alloc(uint32_t size);
uint32_t w32_mem_alloc_zeroed(uint32_t size);
uint32_t w32_mem_realloc(uint32_t addr, uint32_t size);
void     w32_mem_free(uint32_t addr);
uint32_t w32_mem_size(uint32_t addr);
/* Page-granular reservation for VirtualAlloc, which programs expect to
 * return page-aligned memory. */
uint32_t w32_mem_alloc_pages(uint32_t size);

/* Copies a NUL-terminated string into user-visible memory. */
uint32_t w32_strdup_user(const char* s);

/* Resets the whole arena between programs - a process exiting takes all
 * of its allocations with it. */
void w32_mem_reset(void);

/* --- the thunk arena --------------------------------------------------- */

/* Copies `size` bytes of machine code into the user-executable thunk
 * arena and returns its ring-3 address, claiming as many 16-byte thunk
 * slots as it needs. This is the WCODE mechanism (see win32.h) exposed
 * for the callback layer, which plants a two-byte `int 0x82` stub that
 * every kernel-initiated ring-3 callback returns through. Returns 0 if
 * the arena is full. */
uint32_t w32_emit_ring3_code(const uint8_t* code, uint32_t size);

/* --- the data arena ---------------------------------------------------- */

/* Bump allocator for things that live as long as the emulation layer
 * itself: data exports, the TEB/PEB, the fake module headers. */
uint32_t w32_data_alloc(uint32_t size, uint32_t align);

/* --- handles ----------------------------------------------------------- */

#define W32_INVALID_HANDLE_VALUE 0xFFFFFFFFu

/* Standard handles are constants rather than table entries, so that a
 * program which hardcodes them (plenty do) still works. */
#define W32_STDIN_HANDLE  0x10000001u
#define W32_STDOUT_HANDLE 0x10000002u
#define W32_STDERR_HANDLE 0x10000003u
#define W32_PROCESS_HANDLE 0x10000010u
#define W32_THREAD_HANDLE  0x10000011u
#define W32_HEAP_HANDLE    0x10000020u

typedef enum {
    W32_H_FREE = 0,
    W32_H_FILE,
    W32_H_FIND,
    W32_H_EVENT,
} w32_handle_type_t;

/* An open initrd file, or an in-progress directory enumeration. */
typedef struct {
    w32_handle_type_t type;
    void*    node;      /* vfs_node_t* for W32_H_FILE */
    uint32_t position;
    uint32_t index;     /* next entry, for W32_H_FIND */
} w32_handle_t;

uint32_t      w32_handle_open(w32_handle_type_t type, void* node);
w32_handle_t* w32_handle_get(uint32_t handle);
void          w32_handle_close(uint32_t handle);
void          w32_handles_reset(void);

/* True for the three console pseudo-handles. */
int w32_is_console_handle(uint32_t handle);

/* --- strings ----------------------------------------------------------- */

/* UTF-16 <-> ASCII. Non-ASCII code units become '?' rather than being
 * dropped, so string lengths stay predictable. Both return the number of
 * characters written, excluding the terminator. */
uint32_t w32_wide_to_ascii(const uint16_t* src, char* dst, uint32_t dst_size);
uint32_t w32_ascii_to_wide(const char* src, uint16_t* dst, uint32_t dst_chars);
uint32_t w32_wide_len(const uint16_t* s);

/* Copies a wide string into a kernel scratch buffer, for APIs that take
 * a W-form path or name. The buffer is reused per call. */
const char* w32_wide_arg(const uint16_t* s);

/* --- the printf engine (win32_msvcrt.c, win32_dtoa.c) ------------------ */

/* Renders one IEEE-754 double - passed as its raw 64 bits, never as a C
 * `double`; see the top of win32_dtoa.c for why that matters - in %f, %e
 * or %g form. `conv` is the conversion character, uppercase selecting
 * uppercase output; `alternate` is printf's '#' flag. Emits the magnitude
 * only: the sign bit is the caller's business. */
uint32_t w32_format_double(char* out, uint32_t out_size, uint64_t bits,
                           int precision, char conv, int alternate);


/* Formats into `out`, reading varargs straight off the caller's ring-3
 * stack starting at `argp`. Returns the number of characters that would
 * have been written, C99-style, so callers can detect truncation.
 * Implemented once and shared by printf, sprintf, wsprintfA and friends. */
uint32_t w32_format(char* out, uint32_t out_size, const char* fmt,
                    const uint32_t* argp);

/* --- threads (Milestone 17) --------------------------------------------
 *
 * A Win32 program runs as one or more scheduler tasks in a single address
 * space - which is precisely the definition of threads that Milestone 16
 * built. These are the pieces the kernel32 implementations need. */

/* Creates a thread of the running program: its own stack out of the Win32
 * arena, its own TEB, and a scheduler task starting at `start` with
 * `param` as its single stdcall argument. Returns a handle, or 0. The
 * thread's id comes back through `out_tid` when that is non-null. */
uint32_t w32_thread_create(uint32_t start, uint32_t param, uint32_t stack_size,
                           uint32_t* out_tid);

/* Ends the calling thread. If it was the last one the whole program ends.
 *
 * Returns normally to its caller, and must: the scheduler records the
 * switch and isr.s's epilogue performs it as this call chain unwinds. The
 * calling *thread* never resumes - its thunk's `ret` is never reached -
 * but the C function does return, so this is deliberately not marked
 * noreturn. */
void w32_thread_exit(uint32_t exit_code);

/* Makes the emulated call that is in flight re-execute from the top of
 * its thunk next time this thread is scheduled, then gives up the rest of
 * the slice.
 *
 * This is how waiting works without the scheduler being able to block a
 * task in the middle of a syscall and resume its kernel stack later. The
 * thunk is `mov eax, id / int 0x81 / ret n`, so rewinding eip by the 7
 * bytes of the first two instructions makes the whole call happen again -
 * including the `mov`, which is what puts the slot id back in eax. The
 * API implementation returns normally in the meantime and its return
 * value is discarded, so it must not have done anything it would mind
 * doing again.
 *
 * Honest consequence: a waiting thread stays runnable and burns its
 * slices retrying rather than sleeping. */
void w32_retry_call_later(win32_call_t* call);

/* The same, but only when another task is actually runnable; returns 1 if
 * it rewound and yielded, 0 having changed nothing. Sleep() uses it: with
 * no sibling to run there is nothing to gain by spinning, so it falls
 * back to halting until the timer reaches its deadline. */
int w32_yield_if_others(win32_call_t* call);

/* Whether a thread handle names a thread that has not exited yet. When it
 * has, its exit code comes back through `exit_code`. An unknown handle
 * reads as finished, so waiting on a stale one returns rather than
 * hanging forever. */
int w32_thread_handle_alive(uint32_t handle, uint32_t* exit_code);

/* The calling thread's id - the scheduler PID, which is unique within a
 * run and is what GetCurrentThreadId hands out. */
uint32_t w32_thread_id(void);

/* How many times EnterCriticalSection found a lock already held by
 * another thread during this run. Reported at exit, because "the guarded
 * total came out right" only means something if the lock was contended
 * while it did. */
uint32_t w32_cs_contention_count(void);

/* --- the running program ----------------------------------------------- */

/* The image currently executing, or 0 outside a program. */
const pe_image_t* w32_current_image(void);
const char*       w32_current_name(void);
const char*       w32_current_cmdline(void);

/* Terminates the running program and unwinds to the shell. Never returns. */
void w32_exit_process(uint32_t exit_code) __attribute__((noreturn));

/* The ring-3 esp of the innermost emulated call currently in flight, or 0
 * outside one. The callback layer needs somewhere on the program's stack
 * to build a call frame; an API implementation normally passes
 * win32_call_t.useresp, but w32_exit_process() runs atexit handlers from
 * a context that no longer has the win32_call_t, so it reads this. */
uint32_t w32_current_user_esp(void);

/* Runs the program's atexit handlers, most-recently-registered first, and
 * forgets them. Called once from w32_exit_process(); a second call while
 * the first is still running (a handler that calls exit()) does nothing,
 * which is what the C standard asks for. */
void w32_msvcrt_run_atexit(void);

/* Registers a program-supplied callback the emulation layer honors:
 * atexit handlers and the unhandled-exception filter. */
void     w32_set_exception_filter(uint32_t fn);
uint32_t w32_get_exception_filter(void);

/* Address of the emulated module's fake PE header, for GetModuleHandleA
 * of a DLL name. 0 if the name isn't one of ours. */
uint32_t w32_module_handle(const char* name);
/* Reverse lookup, so GetProcAddress can tell which module a handle is. */
int      w32_module_index_from_handle(uint32_t handle);
/* The thunk (or data) address for one export, by module index. */
uint32_t w32_export_address(int module_index, const char* name, uint16_t ordinal);
/* Index of a module by name, or -1. Aliases resolve to their target. */
int w32_module_index(const char* name);

/* Address of the running program's TEB, for NtCurrentTeb(). */
uint32_t w32_teb_address(void);

/* Per-program state reset, called by win32_run_pe() before the image
 * starts. Anything an API caches in a static (the command-line copy, the
 * TLS slots, the CRT's stdio table) has to be rebuilt for each run,
 * because the memory it points into is freed between programs. */
void w32_kernel32_reset(void);
void w32_msvcrt_reset(void);
/* One-time setup after the export tables have addresses assigned. */
void w32_msvcrt_init(void);

#endif
