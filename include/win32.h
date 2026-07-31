#ifndef WIN32_H
#define WIN32_H

#include <stdint.h>
#include "idt.h"
#include "pe.h"

/* The Win32 emulation layer - what makes a real .exe do something other
 * than fault on its first instruction.
 *
 * ROADMAP.md Milestone 8 lays out the two ways to run Windows binaries:
 * port Wine (Path A) or reimplement the API surface (Path B, what
 * ReactOS does). This is neither, and it is worth being exact about what
 * it is: a *third*, much smaller thing - a native implementation of the
 * subset of Win32 that ordinary console programs actually call, provided
 * directly by the kernel with no DLL files involved anywhere.
 *
 * How a call gets from ring 3 into the kernel:
 *
 *   1. win32_init() walks the module tables in kernel/win32_kernel32.c,
 *      win32_msvcrt.c and win32_user32.c and emits one 16-byte *thunk*
 *      per exported function into a page of user-executable memory:
 *
 *          mov eax, <slot index>
 *          int 0x81
 *          ret <bytes>          ; or a plain `ret` for cdecl exports
 *
 *   2. The PE loader (kernel/pe.c) writes those thunk addresses into the
 *      image's import address table, so `call [__imp__WriteFile]` in the
 *      program lands on the thunk.
 *   3. `int 0x81` traps into win32_dispatch(), which finds the slot from
 *      eax and calls the C implementation with a pointer to the caller's
 *      arguments, still sitting on the ring-3 stack.
 *   4. The thunk's `ret <bytes>` performs the stdcall callee-cleanup the
 *      caller is expecting. This is why every export declares its
 *      argument count: get that number wrong and the caller's stack is
 *      silently corrupted, which is far nastier to debug than a missing
 *      function.
 *
 * Data exports (msvcrt's `_iob`, `_fmode`, ...) can't be thunks - the
 * program reads and writes them as variables - so those get real backing
 * storage in the Win32 data arena instead, and the IAT gets the address
 * of that storage.
 *
 * What this deliberately is not: a Windows kernel. There are no real
 * DLLs, one process at a time, no registry, no window manager, no GDI,
 * no sockets. (Threads *are* real since Milestone 17: a program's main
 * thread is a scheduler task, CreateThread adds more of them in the same
 * address space, and critical sections genuinely lock.) An .exe that
 * needs any of the missing pieces gets an honest diagnostic naming the
 * API it wanted - see win32_report() - not a silent hang. `winapi` in the shell lists exactly what is provided. */

/* Ring-3 entry vector for the emulation layer, separate from the OS's own
 * `int 0x80` so the two calling conventions never have to share a
 * dispatcher. Like 0x80 its IDT gate is DPL=3. */
#define WIN32_INT_VECTOR 0x81

/* The address-space layout a Win32 program sees. All of it is below
 * 0xC0000000 (kernel space) and clear of the flat-binary/ELF load address
 * (0x40000000), the mmap arena (0x42000000) and the multitask demo
 * addresses (0x50000000). paging_reserve_region() is told about each of
 * these at init so the PE loader will never place an image on top of one. */
#define WIN32_THUNK_BASE  0x7B000000u
#define WIN32_THUNK_SIZE  0x00040000u /* 256KB = room for 16384 thunks */
#define WIN32_DATA_BASE   0x7C000000u
#define WIN32_DATA_SIZE   0x00100000u /* TEB/PEB, data exports, strings */
#define WIN32_HEAP_BASE   0x60000000u
#define WIN32_HEAP_SIZE   0x08000000u /* 128MB of address space, mapped lazily */
#define WIN32_STACK_TOP   0x5F000000u
#define WIN32_STACK_SIZE  0x00100000u /* 1MB, matching the usual PE default */

/* Arguments of an emulated call, as seen from the kernel. `args` points
 * straight at the caller's ring-3 stack, with args[0] the first argument.
 *
 * The kernel can read it directly because it is standing in the program's
 * own address space while servicing the call and never switches CR3 while
 * holding a pointer into it - see the long comment in process.c. Through
 * Milestone 14 the reason was simpler and weaker: there was only one
 * address space. */
typedef struct win32_call {
    registers_t* regs;
    const uint32_t* args;
    /* The ring-3 esp at the moment this call trapped, copied *by value*
     * at the top of win32_dispatch().
     *
     * This is not a convenience: `regs` points into the trap frame on the
     * kernel stack, and since Milestone 13 a Win32 API implementation can
     * call back into ring 3 (win32_callback.h). Every ring3 -> ring0
     * transition loads esp from the same fixed TSS.esp0, so a trap taken
     * during that callback pushes its frame at exactly the addresses this
     * one occupies. win32_callback.c keeps that from happening by
     * lowering esp0 around the callback - but code that wants the
     * caller's stack pointer should still read this snapshot rather than
     * regs->useresp, because it is the value that is true for *this*
     * call regardless of what happens underneath it. */
    uint32_t useresp;
} win32_call_t;

typedef uint32_t (*win32_fn_t)(win32_call_t* call);

/* One exported symbol. Built through the W* macros below rather than by
 * hand - see kernel/win32_kernel32.c for what a table looks like. */
typedef struct win32_export {
    const char* name;
    win32_fn_t  fn;        /* 0 for data exports, code exports and stubs */
    int16_t     argc;      /* stack dwords to pop; WIN32_CDECL for cdecl */
    uint16_t    ordinal;   /* 0 = no fixed ordinal */
    uint32_t    stub_ret;  /* what a stub (fn == 0) returns */
    uint16_t    data_size; /* > 0 makes this a data export */
    uint16_t    flags;
    /* Machine code copied verbatim into the thunk arena and called
     * directly by the program, instead of trapping into the kernel. This
     * exists for the handful of APIs that must run *user* code: _initterm
     * walks an array of function pointers and calls each one, and the
     * kernel has no way to call back into ring 3 mid-syscall. */
    const uint8_t* code;
    uint16_t    code_size;
} win32_export_t;

#define WIN32_CDECL ((int16_t)-1)

/* Marks an export as present-but-not-really-implemented, so `winapi` can
 * distinguish "we do this" from "we return a plausible value and hope". */
#define WIN32_EXPORT_STUB 0x0001

/* A real implementation, stdcall with `argc` dword arguments. */
#define WEXP(name, argc, fn)   { name, fn, (int16_t)(argc), 0, 0, 0, 0, 0, 0 }
/* A real implementation, cdecl (caller cleans up) - the CRT convention. */
#define WEXP_C(name, fn)       { name, fn, WIN32_CDECL, 0, 0, 0, 0, 0, 0 }
/* Same, but pinned to an ordinal as well as a name. */
#define WEXP_O(name, argc, fn, ord) { name, fn, (int16_t)(argc), (ord), 0, 0, 0, 0, 0 }
/* A stub: returns `ret` and does nothing else. Still needs the right
 * argc, because the thunk's stack cleanup depends on it. */
#define WSTUB(name, argc, ret) { name, 0, (int16_t)(argc), 0, (ret), 0, WIN32_EXPORT_STUB, 0, 0 }
#define WSTUB_C(name, ret)     { name, 0, WIN32_CDECL, 0, (ret), 0, WIN32_EXPORT_STUB, 0, 0 }
/* A variable rather than a function: `size` bytes of zeroed storage in
 * the data arena, whose address goes into the IAT. */
#define WDATA(name, size)      { name, 0, 0, 0, 0, (uint16_t)(size), 0, 0, 0 }
/* A ring-3 native implementation: `blob` must be an array in scope. */
#define WCODE(name, blob)      { name, 0, WIN32_CDECL, 0, 0, 0, 0, blob, (uint16_t)sizeof(blob) }

typedef struct win32_module {
    const char* name;               /* lowercase, including ".dll" */
    const win32_export_t* exports;
    uint32_t count;
    /* Non-null means "this DLL is another name for that one": the UCRT
     * splits msvcrt across dozens of api-ms-win-crt-*.dll names, and
     * every one of them has to resolve to the same implementations. */
    const char* alias_of;
} win32_module_t;

/* Builds the thunk arena and the module registry. Call once at boot,
 * after paging and the kernel heap are up. */
void win32_init(void);

/* Runs a PE image as a Win32 process and blocks until it exits, the same
 * way process_run_user_mode() does for a flat binary. Returns PE_OK once
 * the program has run, or the reason it couldn't be loaded. The exit code
 * comes back through `exit_code` (which may be null) rather than the
 * return value, because a program is free to exit with any 32-bit value
 * including ones that would collide with an error code.
 *
 * `strict` refuses to start a program that has any unresolved import,
 * instead of letting it run until it calls one. */
pe_load_result_t win32_run_pe(const uint8_t* image, uint32_t size,
                              const char* name, const char* cmdline,
                              int strict, uint32_t* exit_code);

/* True while a Win32 program is running in ring 3. */
int win32_process_active(void);

/* True if the last program run ended in an unhandled exception rather
 * than by exiting. The exit code in that case is the exception code, so
 * a caller that reports exit codes should say nothing further - the fault
 * report has already been printed in full. */
int win32_last_run_faulted(void);

/* Called from the CPU-exception path for a fault that happened in ring 3
 * while a Win32 program was running: prints a Windows-style unhandled
 * exception report and terminates the program back to the shell. Returns
 * 1 if it took ownership of the fault (and does not return at all in
 * that case - it unwinds to the shell), 0 to let the panic path run. */
int win32_handle_user_fault(registers_t* regs);

/* Module/export enumeration, for the `winapi` shell command. */
uint32_t    win32_module_count(void);
const char* win32_module_name(uint32_t index);
uint32_t    win32_module_exports(uint32_t index);
uint32_t    win32_module_implemented(uint32_t index);
/* Prints one module's exports through the console, `implemented_only`
 * filtering out the stubs. */
void        win32_list_exports(uint32_t index, int implemented_only);

/* Whether an import would resolve, without loading anything - `peinfo`
 * uses this to show a per-symbol resolved/missing breakdown. */
int win32_have_export(const char* dll, const char* name, uint16_t ordinal);

#endif
