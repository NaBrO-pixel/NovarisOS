/* win32_kernel32.c - kernel32.dll and ntdll.dll.
 *
 * This is the bulk of what an ordinary Windows program calls: console
 * I/O, files, the process heap, virtual memory, time, error codes, and
 * the small pile of initialization APIs every C runtime pokes at before
 * main() runs.
 *
 * Every entry in the tables at the bottom declares its stdcall argument
 * count, and that number is load-bearing: the thunk in win32.c pops
 * exactly that many bytes on the way back (see include/win32.h). An entry
 * with the wrong count corrupts the caller's stack rather than failing
 * visibly, so when adding one, check it against the real prototype. */

#include "win32_internal.h"
#include "console.h"
#include "kstring.h"
#include "pit.h"
#include "pmm.h"
#include "rtc.h"
#include "vfs.h"
#include "kheap.h"

#define ARG(n) (call->args[n])

/* --- console ----------------------------------------------------------- */

#define STD_INPUT_HANDLE  ((uint32_t)-10)
#define STD_OUTPUT_HANDLE ((uint32_t)-11)
#define STD_ERROR_HANDLE  ((uint32_t)-12)

static uint32_t k32_GetStdHandle(win32_call_t* call) {
    switch (ARG(0)) {
        case STD_INPUT_HANDLE:  return W32_STDIN_HANDLE;
        case STD_OUTPUT_HANDLE: return W32_STDOUT_HANDLE;
        case STD_ERROR_HANDLE:  return W32_STDERR_HANDLE;
        default: return W32_INVALID_HANDLE_VALUE;
    }
}

static uint32_t k32_SetStdHandle(win32_call_t* call) {
    (void)call;
    return 1; /* redirecting the standard handles isn't supported */
}

/* WriteFile(handle, buffer, count, written*, overlapped*) - the console
 * handles write to the terminal; anything else fails, since the initrd is
 * read-only (see ROADMAP.md Milestone 6). */
static uint32_t k32_WriteFile(win32_call_t* call) {
    uint32_t handle = ARG(0);
    const char* buffer = (const char*)ARG(1);
    uint32_t count = ARG(2);
    uint32_t* written = (uint32_t*)ARG(3);

    if (!w32_is_console_handle(handle)) {
        w32_set_error(ERROR_ACCESS_DENIED);
        if (written) *written = 0;
        return 0;
    }

    w32_write(buffer, count);
    if (written) *written = count;
    return 1;
}

/* WriteConsoleA(handle, buffer, chars, written*, reserved) */
static uint32_t k32_WriteConsoleA(win32_call_t* call) {
    const char* buffer = (const char*)ARG(1);
    uint32_t count = ARG(2);
    uint32_t* written = (uint32_t*)ARG(3);

    w32_write(buffer, count);
    if (written) *written = count;
    return 1;
}

static uint32_t k32_WriteConsoleW(win32_call_t* call) {
    const uint16_t* buffer = (const uint16_t*)ARG(1);
    uint32_t count = ARG(2);
    uint32_t* written = (uint32_t*)ARG(3);

    /* Converted a chunk at a time rather than all at once, so a long
     * string doesn't need a matching kernel buffer. */
    char chunk[128];
    uint32_t done = 0;
    while (done < count) {
        uint32_t n = count - done;
        if (n > sizeof(chunk) - 1) n = sizeof(chunk) - 1;
        for (uint32_t i = 0; i < n; i++) {
            uint16_t c = buffer[done + i];
            chunk[i] = c < 128 ? (char)c : '?';
        }
        w32_write(chunk, n);
        done += n;
    }
    if (written) *written = count;
    return 1;
}

/* ReadFile(handle, buffer, count, read*, overlapped*) */
static uint32_t k32_ReadFile(win32_call_t* call) {
    uint32_t handle = ARG(0);
    char* buffer = (char*)ARG(1);
    uint32_t count = ARG(2);
    uint32_t* read = (uint32_t*)ARG(3);

    if (handle == W32_STDIN_HANDLE) {
        uint32_t n = w32_read_line(buffer, count);
        if (read) *read = n;
        return 1;
    }

    w32_handle_t* h = w32_handle_get(handle);
    if (!h || h->type != W32_H_FILE) {
        w32_set_error(ERROR_INVALID_HANDLE);
        return 0;
    }

    vfs_node_t* node = (vfs_node_t*)h->node;
    uint32_t remaining = node->length > h->position ? node->length - h->position : 0;
    if (count > remaining) count = remaining;
    uint32_t n = count ? vfs_read(node, h->position, count, (uint8_t*)buffer) : 0;
    h->position += n;
    if (read) *read = n;
    return 1;
}

static uint32_t k32_ReadConsoleA(win32_call_t* call) {
    char* buffer = (char*)ARG(1);
    uint32_t count = ARG(2);
    uint32_t* read = (uint32_t*)ARG(3);
    uint32_t n = w32_read_line(buffer, count);
    if (read) *read = n;
    return 1;
}

static uint32_t k32_SetConsoleTextAttribute(win32_call_t* call) {
    w32_set_attribute((uint16_t)ARG(1));
    return 1;
}

static uint32_t k32_GetConsoleMode(win32_call_t* call) {
    uint32_t* mode = (uint32_t*)ARG(1);
    if (!w32_is_console_handle(ARG(0))) {
        w32_set_error(ERROR_INVALID_HANDLE);
        return 0;
    }
    /* ENABLE_PROCESSED_INPUT|ENABLE_LINE_INPUT|ENABLE_ECHO_INPUT for
     * input; ENABLE_PROCESSED_OUTPUT|ENABLE_WRAP_AT_EOL for output. Both
     * describe what w32_read_line/w32_write actually do. */
    if (mode) *mode = (ARG(0) == W32_STDIN_HANDLE) ? 0x0007 : 0x0003;
    return 1;
}

/* GetConsoleScreenBufferInfo(handle, CONSOLE_SCREEN_BUFFER_INFO*) - the
 * struct is COORD size; COORD cursor; WORD attributes; SMALL_RECT window;
 * COORD maxWindowSize, all 16-bit fields. */
static uint32_t k32_GetConsoleScreenBufferInfo(win32_call_t* call) {
    uint16_t* info = (uint16_t*)ARG(1);
    if (!info) return 0;

    info[0] = 80; info[1] = 25;  /* dwSize */
    info[2] = 0;  info[3] = 0;   /* dwCursorPosition - not tracked */
    info[4] = w32_get_attribute();
    info[5] = 0; info[6] = 0; info[7] = 79; info[8] = 24; /* srWindow */
    info[9] = 80; info[10] = 25; /* dwMaximumWindowSize */
    return 1;
}

/* --- errors ------------------------------------------------------------ */

static uint32_t k32_GetLastError(win32_call_t* call) {
    (void)call;
    return w32_get_error();
}

static uint32_t k32_SetLastError(win32_call_t* call) {
    w32_set_error(ARG(0));
    return 0;
}

/* FormatMessageA(flags, source, id, langid, buffer, size, args) */
static uint32_t k32_FormatMessageA(win32_call_t* call) {
    char* buffer = (char*)ARG(4);
    uint32_t size = ARG(5);
    if (!buffer || size == 0) return 0;

    const char* text = "Novaris: no message text for this error code.";
    uint32_t n = kstrlen(text);
    if (n >= size) n = size - 1;
    kmemcpy(buffer, text, n);
    buffer[n] = '\0';
    return n;
}

/* --- process & modules -------------------------------------------------- */

static uint32_t k32_ExitProcess(win32_call_t* call) {
    w32_exit_process(ARG(0)); /* does not return */
    return 0;
}

static uint32_t k32_TerminateProcess(win32_call_t* call) {
    w32_exit_process(ARG(1));
    return 1;
}

static uint32_t k32_GetCurrentProcess(win32_call_t* call) {
    (void)call;
    return W32_PROCESS_HANDLE;
}

static uint32_t k32_GetCurrentThread(win32_call_t* call) {
    (void)call;
    return W32_THREAD_HANDLE;
}

static uint32_t k32_GetCurrentProcessId(win32_call_t* call) {
    (void)call;
    return 0x100;
}

static uint32_t k32_GetCurrentThreadId(win32_call_t* call) {
    (void)call;
    return 0x104;
}

static uint32_t k32_GetModuleHandleA(win32_call_t* call) {
    const char* name = (const char*)ARG(0);
    /* NULL means "the running program", which is the one module whose
     * base is a genuinely mapped PE image rather than a stand-in. */
    if (!name) {
        const pe_image_t* img = w32_current_image();
        return img ? img->base : 0;
    }
    uint32_t h = w32_module_handle(name);
    if (!h) w32_set_error(ERROR_FILE_NOT_FOUND);
    return h;
}

static uint32_t k32_GetModuleHandleW(win32_call_t* call) {
    const uint16_t* wide = (const uint16_t*)ARG(0);
    if (!wide) {
        const pe_image_t* img = w32_current_image();
        return img ? img->base : 0;
    }
    return w32_module_handle(w32_wide_arg(wide));
}

/* GetModuleHandleExA(flags, name, module**) */
static uint32_t k32_GetModuleHandleExA(win32_call_t* call) {
    const char* name = (const char*)ARG(1);
    uint32_t* out = (uint32_t*)ARG(2);
    uint32_t h;
    if (!name) {
        const pe_image_t* img = w32_current_image();
        h = img ? img->base : 0;
    } else {
        h = w32_module_handle(name);
    }
    if (out) *out = h;
    return h != 0;
}

static uint32_t k32_LoadLibraryA(win32_call_t* call) {
    const char* name = (const char*)ARG(0);
    uint32_t h = name ? w32_module_handle(name) : 0;
    if (!h) {
        /* Honest failure rather than a fake handle: a program that gets a
         * handle back will call GetProcAddress on it and dereference the
         * result. Failing here sends it down its own fallback path. */
        w32_set_error(ERROR_FILE_NOT_FOUND);
    }
    return h;
}

static uint32_t k32_LoadLibraryW(win32_call_t* call) {
    const uint16_t* wide = (const uint16_t*)ARG(0);
    uint32_t h = wide ? w32_module_handle(w32_wide_arg(wide)) : 0;
    if (!h) w32_set_error(ERROR_FILE_NOT_FOUND);
    return h;
}

static uint32_t k32_LoadLibraryExA(win32_call_t* call) {
    const char* name = (const char*)ARG(0);
    uint32_t h = name ? w32_module_handle(name) : 0;
    if (!h) w32_set_error(ERROR_FILE_NOT_FOUND);
    return h;
}

/* GetProcAddress(module, name) - the other half of what makes delay-loaded
 * imports work: the CRT's delay-load helper calls LoadLibrary then this. */
static uint32_t k32_GetProcAddress(win32_call_t* call) {
    uint32_t module = ARG(0);
    uint32_t name_arg = ARG(1);

    int index = w32_module_index_from_handle(module);
    if (index < 0) {
        w32_set_error(ERROR_INVALID_HANDLE);
        return 0;
    }

    /* A name argument with a zero high word is an ordinal, not a pointer -
     * the MAKEINTRESOURCE convention. */
    uint32_t addr;
    if (name_arg <= 0xFFFF) {
        addr = w32_export_address(index, 0, (uint16_t)name_arg);
    } else {
        addr = w32_export_address(index, (const char*)name_arg, 0);
    }
    if (!addr) w32_set_error(ERROR_FILE_NOT_FOUND);
    return addr;
}

static uint32_t k32_FreeLibrary(win32_call_t* call) {
    (void)call;
    return 1;
}

/* GetModuleFileNameA(module, buffer, size) */
static uint32_t k32_GetModuleFileNameA(win32_call_t* call) {
    char* buffer = (char*)ARG(1);
    uint32_t size = ARG(2);
    if (!buffer || size == 0) return 0;

    char path[128];
    kstrlcpy(path, "C:\\Novaris\\", sizeof(path));
    kstrcat(path, w32_current_name());

    uint32_t n = kstrlen(path);
    if (n >= size) n = size - 1;
    kmemcpy(buffer, path, n);
    buffer[n] = '\0';
    return n;
}

static uint32_t k32_GetModuleFileNameW(win32_call_t* call) {
    uint16_t* buffer = (uint16_t*)ARG(1);
    uint32_t size = ARG(2);
    if (!buffer || size == 0) return 0;

    char path[128];
    kstrlcpy(path, "C:\\Novaris\\", sizeof(path));
    kstrcat(path, w32_current_name());
    return w32_ascii_to_wide(path, buffer, size);
}

/* The command line lives in user memory for the lifetime of the process,
 * because programs keep the pointer and read it repeatedly. */
static uint32_t cmdline_ansi = 0;
static uint32_t cmdline_wide = 0;

static uint32_t k32_GetCommandLineA(win32_call_t* call) {
    (void)call;
    if (!cmdline_ansi) cmdline_ansi = w32_strdup_user(w32_current_cmdline());
    return cmdline_ansi;
}

static uint32_t k32_GetCommandLineW(win32_call_t* call) {
    (void)call;
    if (!cmdline_wide) {
        const char* s = w32_current_cmdline();
        uint32_t chars = kstrlen(s) + 1;
        cmdline_wide = w32_mem_alloc(chars * 2);
        if (cmdline_wide) w32_ascii_to_wide(s, (uint16_t*)cmdline_wide, chars);
    }
    return cmdline_wide;
}

/* STARTUPINFOA is 68 bytes on 32-bit; only cb and the flags matter to a
 * CRT, which checks them and moves on. */
static uint32_t k32_GetStartupInfoA(win32_call_t* call) {
    uint32_t* si = (uint32_t*)ARG(0);
    if (!si) return 0;
    kmemset(si, 0, 68);
    si[0] = 68;                       /* cb */
    si[11] = 0;                       /* dwFlags: nothing specified */
    ((uint32_t*)si)[14] = W32_STDIN_HANDLE;
    ((uint32_t*)si)[15] = W32_STDOUT_HANDLE;
    ((uint32_t*)si)[16] = W32_STDERR_HANDLE;
    return 0;
}

/* The environment block is a run of NUL-terminated NAME=VALUE strings
 * ending in an extra NUL. */
static const char env_block_ansi[] = "PATH=C:\\Novaris\0OS=Novaris\0COMPUTERNAME=NOVARIS\0";

static uint32_t k32_GetEnvironmentStrings(win32_call_t* call) {
    (void)call;
    uint32_t p = w32_mem_alloc(sizeof(env_block_ansi));
    if (p) kmemcpy((void*)p, env_block_ansi, sizeof(env_block_ansi));
    return p;
}

static uint32_t k32_GetEnvironmentStringsW(win32_call_t* call) {
    (void)call;
    uint32_t chars = sizeof(env_block_ansi);
    uint32_t p = w32_mem_alloc(chars * 2);
    if (!p) return 0;
    uint16_t* dst = (uint16_t*)p;
    for (uint32_t i = 0; i < chars; i++) dst[i] = (uint16_t)(uint8_t)env_block_ansi[i];
    return p;
}

static uint32_t k32_FreeEnvironmentStrings(win32_call_t* call) {
    w32_mem_free(ARG(0));
    return 1;
}

/* GetEnvironmentVariableA(name, buffer, size) */
static uint32_t k32_GetEnvironmentVariableA(win32_call_t* call) {
    const char* name = (const char*)ARG(0);
    char* buffer = (char*)ARG(1);
    uint32_t size = ARG(2);
    if (!name) return 0;

    for (const char* e = env_block_ansi; *e; e += kstrlen(e) + 1) {
        const char* eq = kstrchr(e, '=');
        if (!eq) continue;
        uint32_t namelen = (uint32_t)(eq - e);
        if (kstrnlen(name, namelen + 1) != namelen) continue;
        if (kstrncmp(name, e, namelen) != 0) continue;

        const char* value = eq + 1;
        uint32_t vlen = kstrlen(value);
        if (!buffer || size <= vlen) return vlen + 1; /* required size */
        kmemcpy(buffer, value, vlen + 1);
        return vlen;
    }
    w32_set_error(203); /* ERROR_ENVVAR_NOT_FOUND */
    return 0;
}

static uint32_t k32_Sleep(win32_call_t* call) {
    /* Busy-waits on the PIT tick counter. Interrupts are off inside an
     * interrupt gate, so the counter would never advance - re-enable them
     * for the duration, which is safe here because nothing in this call
     * holds kernel state that an IRQ handler touches. */
    uint32_t ms = ARG(0);
    uint32_t target = pit_get_ticks() + (ms + 9) / 10; /* the PIT runs at 100Hz */
    __asm__ __volatile__("sti");
    while (pit_get_ticks() < target) {
        __asm__ __volatile__("hlt");
    }
    __asm__ __volatile__("cli");
    return 0;
}

static uint32_t k32_SetUnhandledExceptionFilter(win32_call_t* call) {
    uint32_t previous = w32_get_exception_filter();
    w32_set_exception_filter(ARG(0));
    return previous;
}

static uint32_t k32_GetSystemInfo(win32_call_t* call) {
    uint32_t* si = (uint32_t*)ARG(0);
    if (!si) return 0;
    kmemset(si, 0, 36);
    si[0] = 0;          /* wProcessorArchitecture = INTEL, wReserved */
    si[1] = 4096;       /* dwPageSize */
    si[2] = 0x10000;    /* lpMinimumApplicationAddress */
    si[3] = 0x7FFEFFFF; /* lpMaximumApplicationAddress */
    si[4] = 1;          /* dwActiveProcessorMask */
    si[5] = 1;          /* dwNumberOfProcessors */
    si[6] = 586;        /* dwProcessorType = PROCESSOR_INTEL_PENTIUM */
    si[7] = 0x10000;    /* dwAllocationGranularity */
    si[8] = 5 | (6 << 8); /* wProcessorLevel | wProcessorRevision */
    return 0;
}

static uint32_t k32_GetVersion(win32_call_t* call) {
    (void)call;
    /* Windows 7 (6.1), build 7601, NT platform - the low word is
     * minor<<8|major and the high word is the build. */
    return (7601u << 16) | (1u << 8) | 6u;
}

/* OSVERSIONINFOA: dwOSVersionInfoSize, major, minor, build, platformId,
 * szCSDVersion[128]. */
static uint32_t k32_GetVersionExA(win32_call_t* call) {
    uint32_t* v = (uint32_t*)ARG(0);
    if (!v || v[0] < 20) return 0;
    v[1] = 6;
    v[2] = 1;
    v[3] = 7601;
    v[4] = 2; /* VER_PLATFORM_WIN32_NT */
    kstrcpy((char*)&v[5], "Service Pack 1");
    return 1;
}

static uint32_t k32_IsProcessorFeaturePresent(win32_call_t* call) {
    /* 0 = FP_ERRATA, 1 = FP_EMULATED, 2 = COMPARE_EXCHANGE, 3 = MMX,
     * 6 = XMMI(SSE), 10 = XMMI64(SSE2). We're a plain 32-bit i386 target
     * with no SSE guarantee, so only claim the two that always hold. */
    switch (ARG(0)) {
        case 2:  return 1; /* cmpxchg8b */
        case 12: return 0; /* NX */
        default: return 0;
    }
}

/* --- heap -------------------------------------------------------------- */

static uint32_t k32_GetProcessHeap(win32_call_t* call) {
    (void)call;
    return W32_HEAP_HANDLE;
}

/* HeapAlloc(heap, flags, size) - flag 0x8 is HEAP_ZERO_MEMORY. */
static uint32_t k32_HeapAlloc(win32_call_t* call) {
    uint32_t flags = ARG(1);
    uint32_t size = ARG(2);
    uint32_t p = (flags & 0x8) ? w32_mem_alloc_zeroed(size) : w32_mem_alloc(size);
    if (!p) w32_set_error(ERROR_NOT_ENOUGH_MEMORY);
    return p;
}

static uint32_t k32_HeapReAlloc(win32_call_t* call) {
    return w32_mem_realloc(ARG(2), ARG(3));
}

static uint32_t k32_HeapFree(win32_call_t* call) {
    w32_mem_free(ARG(2));
    return 1;
}

static uint32_t k32_HeapSize(win32_call_t* call) {
    return w32_mem_size(ARG(2));
}

static uint32_t k32_HeapCreate(win32_call_t* call) {
    (void)call;
    /* One arena serves every "heap": there is no per-heap isolation, but
     * every handle we hand out is one HeapAlloc understands. */
    return W32_HEAP_HANDLE;
}

/* LocalAlloc/GlobalAlloc(flags, size) - flag 0x40 (LPTR/GPTR) zeroes. */
static uint32_t k32_LocalAlloc(win32_call_t* call) {
    uint32_t flags = ARG(0);
    uint32_t size = ARG(1);
    return (flags & 0x40) ? w32_mem_alloc_zeroed(size) : w32_mem_alloc(size);
}

static uint32_t k32_LocalFree(win32_call_t* call) {
    w32_mem_free(ARG(0));
    return 0;
}

static uint32_t k32_LocalReAlloc(win32_call_t* call) {
    return w32_mem_realloc(ARG(0), ARG(1));
}

static uint32_t k32_Identity(win32_call_t* call) {
    /* GlobalLock/LocalLock/EncodePointer and friends: with no handle
     * indirection and no pointer obfuscation, the pointer is the value. */
    return ARG(0);
}

/* --- virtual memory ---------------------------------------------------- */

/* VirtualAlloc(address, size, type, protect) */
static uint32_t k32_VirtualAlloc(win32_call_t* call) {
    uint32_t address = ARG(0);
    uint32_t size = ARG(1);

    /* A non-null address means "commit this reservation", which for us is
     * already-usable memory - the arena has no reserve/commit split. */
    if (address) return address;

    uint32_t p = w32_mem_alloc_pages(size);
    if (!p) w32_set_error(ERROR_NOT_ENOUGH_MEMORY);
    return p;
}

static uint32_t k32_VirtualFree(win32_call_t* call) {
    (void)call;
    return 1; /* page allocations are reclaimed wholesale at process exit */
}

/* VirtualProtect(address, size, newProtect, oldProtect*) - every page a
 * program can see is already RW and executable here (this paging setup
 * has no NX bit), so this only has to report a plausible previous state. */
static uint32_t k32_VirtualProtect(win32_call_t* call) {
    uint32_t* old = (uint32_t*)ARG(3);
    if (old) *old = 0x40; /* PAGE_EXECUTE_READWRITE */
    return 1;
}

/* VirtualQuery(address, MEMORY_BASIC_INFORMATION*, length) */
static uint32_t k32_VirtualQuery(win32_call_t* call) {
    uint32_t address = ARG(0);
    uint32_t* mbi = (uint32_t*)ARG(1);
    uint32_t length = ARG(2);
    if (!mbi || length < 28) return 0;

    mbi[0] = address & ~0xFFFu; /* BaseAddress */
    mbi[1] = address & ~0xFFFu; /* AllocationBase */
    mbi[2] = 0x40;              /* AllocationProtect */
    mbi[3] = 4096;              /* RegionSize */
    mbi[4] = 0x1000;            /* State = MEM_COMMIT */
    mbi[5] = 0x40;              /* Protect */
    mbi[6] = 0x20000;           /* Type = MEM_PRIVATE */
    return 28;
}

static uint32_t k32_GlobalMemoryStatus(win32_call_t* call) {
    uint32_t* ms = (uint32_t*)ARG(0);
    if (!ms) return 0;
    uint32_t total = pmm_total_frames() * 4096u;
    uint32_t avail = pmm_free_frames() * 4096u;
    ms[0] = 32;      /* dwLength */
    ms[1] = total ? (100u - (avail * 100u) / total) : 0; /* dwMemoryLoad */
    ms[2] = total;
    ms[3] = avail;
    ms[4] = total;   /* page file - no swap, so the same numbers */
    ms[5] = avail;
    ms[6] = WIN32_HEAP_SIZE;
    ms[7] = WIN32_HEAP_SIZE;
    return 0;
}

static uint32_t k32_IsBadPtr(win32_call_t* call) {
    /* Returns non-zero for a *bad* pointer. Only null is definitely bad
     * here - claiming anything else is unreadable would send callers down
     * error paths for memory that works fine. */
    return ARG(0) == 0;
}

/* --- synchronization --------------------------------------------------- */

/* One thread means a critical section never contends. The struct still
 * gets initialized, because programs read RecursionCount and OwningThread
 * out of it. */
static uint32_t k32_InitializeCriticalSection(win32_call_t* call) {
    uint32_t* cs = (uint32_t*)ARG(0);
    if (cs) kmemset(cs, 0, 24);
    return 0;
}

static uint32_t k32_InitializeCriticalSectionAndSpinCount(win32_call_t* call) {
    uint32_t* cs = (uint32_t*)ARG(0);
    if (cs) kmemset(cs, 0, 24);
    return 1;
}

static uint32_t k32_CriticalSectionNop(win32_call_t* call) {
    (void)call;
    return 0;
}

static uint32_t k32_TryEnterCriticalSection(win32_call_t* call) {
    (void)call;
    return 1; /* uncontended, always */
}

#define MAX_TLS_SLOTS 64
static uint32_t tls_slots[MAX_TLS_SLOTS];
static uint8_t tls_used[MAX_TLS_SLOTS];

static uint32_t k32_TlsAlloc(win32_call_t* call) {
    (void)call;
    for (uint32_t i = 0; i < MAX_TLS_SLOTS; i++) {
        if (!tls_used[i]) {
            tls_used[i] = 1;
            tls_slots[i] = 0;
            return i;
        }
    }
    return 0xFFFFFFFFu; /* TLS_OUT_OF_INDEXES */
}

static uint32_t k32_TlsGetValue(win32_call_t* call) {
    uint32_t i = ARG(0);
    if (i >= MAX_TLS_SLOTS) { w32_set_error(ERROR_INVALID_PARAMETER); return 0; }
    w32_set_error(ERROR_SUCCESS); /* documented: required for the 0 case */
    return tls_slots[i];
}

static uint32_t k32_TlsSetValue(win32_call_t* call) {
    uint32_t i = ARG(0);
    if (i >= MAX_TLS_SLOTS) return 0;
    tls_slots[i] = ARG(1);
    return 1;
}

static uint32_t k32_TlsFree(win32_call_t* call) {
    uint32_t i = ARG(0);
    if (i >= MAX_TLS_SLOTS) return 0;
    tls_used[i] = 0;
    return 1;
}

/* The Interlocked* family. With one thread and interrupts already
 * disabled inside the dispatcher, plain reads and writes *are* atomic
 * here - the guarantee these APIs promise is met, just trivially. */
static uint32_t k32_InterlockedIncrement(win32_call_t* call) {
    int32_t* p = (int32_t*)ARG(0);
    return p ? (uint32_t)(++(*p)) : 0;
}

static uint32_t k32_InterlockedDecrement(win32_call_t* call) {
    int32_t* p = (int32_t*)ARG(0);
    return p ? (uint32_t)(--(*p)) : 0;
}

static uint32_t k32_InterlockedExchange(win32_call_t* call) {
    uint32_t* p = (uint32_t*)ARG(0);
    if (!p) return 0;
    uint32_t old = *p;
    *p = ARG(1);
    return old;
}

static uint32_t k32_InterlockedExchangeAdd(win32_call_t* call) {
    uint32_t* p = (uint32_t*)ARG(0);
    if (!p) return 0;
    uint32_t old = *p;
    *p = old + ARG(1);
    return old;
}

static uint32_t k32_InterlockedCompareExchange(win32_call_t* call) {
    uint32_t* p = (uint32_t*)ARG(0);
    if (!p) return 0;
    uint32_t old = *p;
    if (old == ARG(2)) *p = ARG(1);
    return old;
}

/* InitOnceExecuteOnce(once, fn, param, context) - the callback is ring-3
 * code, which the kernel can't call into from here. Reporting failure
 * makes the caller run its own fallback rather than silently skipping
 * initialization it needed. */
static uint32_t k32_InitOnceExecuteOnce(win32_call_t* call) {
    (void)call;
    return 0;
}

/* --- time -------------------------------------------------------------- */

static uint32_t k32_GetTickCount(win32_call_t* call) {
    (void)call;
    return pit_get_ticks() * 10; /* the PIT is installed at 100Hz */
}

static uint32_t k32_QueryPerformanceCounter(win32_call_t* call) {
    uint32_t* counter = (uint32_t*)ARG(0);
    if (!counter) return 0;
    counter[0] = pit_get_ticks();
    counter[1] = 0;
    return 1;
}

static uint32_t k32_QueryPerformanceFrequency(win32_call_t* call) {
    uint32_t* freq = (uint32_t*)ARG(0);
    if (!freq) return 0;
    freq[0] = 100; /* ticks per second, matching the counter above */
    freq[1] = 0;
    return 1;
}

/* SYSTEMTIME: year, month, dayOfWeek, day, hour, minute, second, ms - all
 * 16-bit. */
static void fill_systemtime(uint16_t* st) {
    rtc_time_t t;
    rtc_read(&t);
    st[0] = t.year;
    st[1] = t.month;
    st[2] = t.weekday;
    st[3] = t.day;
    st[4] = t.hour;
    st[5] = t.minute;
    st[6] = t.second;
    st[7] = 0;
}

static uint32_t k32_GetSystemTime(win32_call_t* call) {
    uint16_t* st = (uint16_t*)ARG(0);
    if (st) fill_systemtime(st);
    return 0;
}

/* A FILETIME is 100-nanosecond intervals since 1601-01-01 UTC. */
static void fill_filetime(uint32_t* ft) {
    rtc_time_t t;
    rtc_read(&t);

    int32_t days_1970 = rtc_days_from_civil(t.year, t.month, t.day);
    /* 1601-01-01 is 134774 days before 1970-01-01. */
    uint64_t seconds = (uint64_t)(days_1970 + 134774) * 86400ull +
                       t.hour * 3600ull + t.minute * 60ull + t.second;
    uint64_t intervals = seconds * 10000000ull;
    ft[0] = (uint32_t)(intervals & 0xFFFFFFFFull);
    ft[1] = (uint32_t)(intervals >> 32);
}

static uint32_t k32_GetSystemTimeAsFileTime(win32_call_t* call) {
    uint32_t* ft = (uint32_t*)ARG(0);
    if (ft) fill_filetime(ft);
    return 0;
}

static uint32_t k32_GetTimeZoneInformation(win32_call_t* call) {
    uint32_t* tzi = (uint32_t*)ARG(0);
    if (tzi) kmemset(tzi, 0, 172); /* bias 0: the CMOS clock is treated as UTC */
    return 0; /* TIME_ZONE_ID_UNKNOWN */
}

/* --- files ------------------------------------------------------------- */

/* Strips any drive/directory prefix: the initrd is one flat directory
 * (see ROADMAP.md Milestone 6), so "C:\dir\file.txt" opens "file.txt". */
static const char* basename_of(const char* path) {
    const char* slash = kstrrchr(path, '\\');
    if (slash) return slash + 1;
    slash = kstrrchr(path, '/');
    if (slash) return slash + 1;
    const char* colon = kstrrchr(path, ':');
    return colon ? colon + 1 : path;
}

/* CreateFileA(name, access, share, security, disposition, flags, template) */
static uint32_t k32_CreateFileA(win32_call_t* call) {
    const char* name = (const char*)ARG(0);
    uint32_t access = ARG(1);
    if (!name) {
        w32_set_error(ERROR_INVALID_PARAMETER);
        return W32_INVALID_HANDLE_VALUE;
    }

    if (kstricmp(name, "CONOUT$") == 0) return W32_STDOUT_HANDLE;
    if (kstricmp(name, "CONIN$") == 0)  return W32_STDIN_HANDLE;

    /* GENERIC_WRITE is 0x40000000. The initrd is read-only, so anything
     * asking to write gets told so up front rather than at the first
     * WriteFile. */
    if (access & 0x40000000u) {
        w32_set_error(ERROR_ACCESS_DENIED);
        return W32_INVALID_HANDLE_VALUE;
    }

    vfs_node_t* node = vfs_root ? vfs_finddir(vfs_root, basename_of(name)) : 0;
    if (!node) {
        w32_set_error(ERROR_FILE_NOT_FOUND);
        return W32_INVALID_HANDLE_VALUE;
    }
    return w32_handle_open(W32_H_FILE, node);
}

static uint32_t k32_CreateFileW(win32_call_t* call) {
    const uint16_t* wide = (const uint16_t*)ARG(0);
    if (!wide) return W32_INVALID_HANDLE_VALUE;

    /* Reuses the A-form by substituting a narrowed copy of the path.
     * w32_wide_arg's scratch buffer is safe here: it isn't read again
     * after k32_CreateFileA copies out of it. */
    uint32_t narrowed[7];
    for (int i = 0; i < 7; i++) narrowed[i] = call->args[i];
    narrowed[0] = (uint32_t)w32_wide_arg(wide);

    win32_call_t inner;
    inner.regs = call->regs;
    inner.args = narrowed;
    return k32_CreateFileA(&inner);
}

static uint32_t k32_CloseHandle(win32_call_t* call) {
    uint32_t handle = ARG(0);
    if (w32_is_console_handle(handle)) return 1;
    if (handle == W32_PROCESS_HANDLE || handle == W32_THREAD_HANDLE) return 1;
    w32_handle_close(handle);
    return 1;
}

static uint32_t k32_GetFileSize(win32_call_t* call) {
    w32_handle_t* h = w32_handle_get(ARG(0));
    uint32_t* high = (uint32_t*)ARG(1);
    if (high) *high = 0;
    if (!h || h->type != W32_H_FILE) {
        w32_set_error(ERROR_INVALID_HANDLE);
        return 0xFFFFFFFFu;
    }
    return ((vfs_node_t*)h->node)->length;
}

/* SetFilePointer(handle, distance, distanceHigh*, method) */
static uint32_t k32_SetFilePointer(win32_call_t* call) {
    w32_handle_t* h = w32_handle_get(ARG(0));
    int32_t distance = (int32_t)ARG(1);
    uint32_t method = ARG(3);
    if (!h || h->type != W32_H_FILE) {
        w32_set_error(ERROR_INVALID_HANDLE);
        return 0xFFFFFFFFu;
    }

    uint32_t length = ((vfs_node_t*)h->node)->length;
    int32_t base = (method == 1) ? (int32_t)h->position
                 : (method == 2) ? (int32_t)length
                 : 0;
    int32_t target = base + distance;
    if (target < 0) target = 0;
    if ((uint32_t)target > length) target = (int32_t)length;
    h->position = (uint32_t)target;
    return h->position;
}

static uint32_t k32_GetFileType(win32_call_t* call) {
    uint32_t handle = ARG(0);
    if (w32_is_console_handle(handle)) return 2; /* FILE_TYPE_CHAR */
    return w32_handle_get(handle) ? 1 : 0;       /* FILE_TYPE_DISK / UNKNOWN */
}

static uint32_t k32_GetFileAttributesA(win32_call_t* call) {
    const char* name = (const char*)ARG(0);
    if (!name) return 0xFFFFFFFFu;
    vfs_node_t* node = vfs_root ? vfs_finddir(vfs_root, basename_of(name)) : 0;
    if (!node) {
        w32_set_error(ERROR_FILE_NOT_FOUND);
        return 0xFFFFFFFFu; /* INVALID_FILE_ATTRIBUTES */
    }
    return 0x01; /* FILE_ATTRIBUTE_READONLY */
}

/* WIN32_FIND_DATAA is 320 bytes: attributes, three FILETIMEs, two size
 * words, two reserved words, cFileName[260], cAlternateFileName[14]. */
#define FIND_DATA_NAME_OFFSET 44

static void fill_find_data(uint8_t* data, vfs_node_t* node) {
    kmemset(data, 0, 320);
    *(uint32_t*)data = 0x01; /* FILE_ATTRIBUTE_READONLY */
    *(uint32_t*)(data + 32) = 0; /* nFileSizeHigh */
    *(uint32_t*)(data + 36) = node->length;
    kstrlcpy((char*)(data + FIND_DATA_NAME_OFFSET), node->name, 260);
}

static uint32_t k32_FindFirstFileA(win32_call_t* call) {
    uint8_t* data = (uint8_t*)ARG(1);
    if (!vfs_root || !data) {
        w32_set_error(ERROR_FILE_NOT_FOUND);
        return W32_INVALID_HANDLE_VALUE;
    }

    /* Only "list everything" is supported - there's no wildcard matcher,
     * and a flat read-only initrd makes patterns close to pointless. */
    vfs_node_t* node = vfs_readdir(vfs_root, 0);
    if (!node) {
        w32_set_error(ERROR_FILE_NOT_FOUND);
        return W32_INVALID_HANDLE_VALUE;
    }

    uint32_t handle = w32_handle_open(W32_H_FIND, 0);
    w32_handle_t* h = w32_handle_get(handle);
    if (!h) return W32_INVALID_HANDLE_VALUE;
    h->index = 1;
    fill_find_data(data, node);
    return handle;
}

static uint32_t k32_FindNextFileA(win32_call_t* call) {
    w32_handle_t* h = w32_handle_get(ARG(0));
    uint8_t* data = (uint8_t*)ARG(1);
    if (!h || h->type != W32_H_FIND || !data) {
        w32_set_error(ERROR_INVALID_HANDLE);
        return 0;
    }

    vfs_node_t* node = vfs_readdir(vfs_root, h->index);
    if (!node) {
        w32_set_error(ERROR_NO_MORE_FILES);
        return 0;
    }
    h->index++;
    fill_find_data(data, node);
    return 1;
}

static uint32_t k32_FindClose(win32_call_t* call) {
    w32_handle_close(ARG(0));
    return 1;
}

static uint32_t k32_GetCurrentDirectoryA(win32_call_t* call) {
    uint32_t size = ARG(0);
    char* buffer = (char*)ARG(1);
    const char* cwd = "C:\\Novaris";
    uint32_t n = kstrlen(cwd);
    if (!buffer || size <= n) return n + 1;
    kmemcpy(buffer, cwd, n + 1);
    return n;
}

/* --- character sets ---------------------------------------------------- */

/* MultiByteToWideChar(codepage, flags, src, srclen, dst, dstchars) */
static uint32_t k32_MultiByteToWideChar(win32_call_t* call) {
    const char* src = (const char*)ARG(2);
    int32_t srclen = (int32_t)ARG(3);
    uint16_t* dst = (uint16_t*)ARG(4);
    uint32_t dstchars = ARG(5);
    if (!src) return 0;

    uint32_t n = (srclen < 0) ? kstrlen(src) + 1 : (uint32_t)srclen;
    if (dstchars == 0) return n; /* measuring pass */
    if (n > dstchars) n = dstchars;
    for (uint32_t i = 0; i < n; i++) dst[i] = (uint16_t)(uint8_t)src[i];
    return n;
}

/* WideCharToMultiByte(cp, flags, src, srclen, dst, dstbytes, default, used) */
static uint32_t k32_WideCharToMultiByte(win32_call_t* call) {
    const uint16_t* src = (const uint16_t*)ARG(2);
    int32_t srclen = (int32_t)ARG(3);
    char* dst = (char*)ARG(4);
    uint32_t dstbytes = ARG(5);
    if (!src) return 0;

    uint32_t n = (srclen < 0) ? w32_wide_len(src) + 1 : (uint32_t)srclen;
    if (dstbytes == 0) return n;
    if (n > dstbytes) n = dstbytes;
    for (uint32_t i = 0; i < n; i++) dst[i] = src[i] < 128 ? (char)src[i] : '?';
    return n;
}

static uint32_t k32_GetACP(win32_call_t* call) {
    (void)call;
    return 1252; /* Windows-1252, the usual Western ANSI code page */
}

static uint32_t k32_GetCPInfo(win32_call_t* call) {
    uint8_t* info = (uint8_t*)ARG(1);
    if (!info) return 0;
    kmemset(info, 0, 20);
    *(uint32_t*)info = 1; /* MaxCharSize: single-byte */
    info[4] = '?';        /* DefaultChar */
    return 1;
}

static uint32_t k32_IsValidCodePage(win32_call_t* call) {
    uint32_t cp = ARG(0);
    return cp == 1252 || cp == 437 || cp == 65001 || cp == 0 || cp == 1 || cp == 3;
}

/* GetStringTypeW(type, src, count, out) - only CT_CTYPE1 is answered,
 * which is what the CRT's isalpha/isdigit tables are built from. */
static uint32_t k32_GetStringTypeW(win32_call_t* call) {
    const uint16_t* src = (const uint16_t*)ARG(1);
    int32_t count = (int32_t)ARG(2);
    uint16_t* out = (uint16_t*)ARG(3);
    if (!src || !out) return 0;

    uint32_t n = count < 0 ? w32_wide_len(src) : (uint32_t)count;
    for (uint32_t i = 0; i < n; i++) {
        uint16_t c = src[i];
        uint16_t type = 0;
        if (c >= 'A' && c <= 'Z') type |= 0x0001 | 0x0100; /* UPPER | ALPHA */
        if (c >= 'a' && c <= 'z') type |= 0x0002 | 0x0100; /* LOWER | ALPHA */
        if (c >= '0' && c <= '9') type |= 0x0004;          /* DIGIT */
        if (c == ' ' || (c >= 9 && c <= 13)) type |= 0x0008; /* SPACE */
        if (c < 32 || c == 127) type |= 0x0020;            /* CNTRL */
        if (c == ' ') type |= 0x0040;                       /* BLANK */
        if (c > 32 && c < 127 && !(type & 0x0107)) type |= 0x0010; /* PUNCT */
        out[i] = type;
    }
    return 1;
}

/* LCMapStringA/W(locale, flags, src, srclen, dst, dstlen) - LCMAP_UPPERCASE
 * is 0x200 and LCMAP_LOWERCASE 0x100; anything else copies unchanged. */
static uint32_t k32_LCMapStringA(win32_call_t* call) {
    uint32_t flags = ARG(1);
    const char* src = (const char*)ARG(2);
    int32_t srclen = (int32_t)ARG(3);
    char* dst = (char*)ARG(4);
    uint32_t dstlen = ARG(5);
    if (!src) return 0;

    uint32_t n = srclen < 0 ? kstrlen(src) + 1 : (uint32_t)srclen;
    if (dstlen == 0) return n;
    if (n > dstlen) n = dstlen;
    for (uint32_t i = 0; i < n; i++) {
        char c = src[i];
        if ((flags & 0x200) && c >= 'a' && c <= 'z') c = (char)(c - 32);
        if ((flags & 0x100) && c >= 'A' && c <= 'Z') c = (char)(c + 32);
        dst[i] = c;
    }
    return n;
}

static uint32_t k32_lstrlenA(win32_call_t* call) {
    const char* s = (const char*)ARG(0);
    return s ? kstrlen(s) : 0;
}

static uint32_t k32_lstrlenW(win32_call_t* call) {
    return w32_wide_len((const uint16_t*)ARG(0));
}

static uint32_t k32_lstrcpyA(win32_call_t* call) {
    char* dst = (char*)ARG(0);
    const char* src = (const char*)ARG(1);
    if (!dst || !src) return 0;
    kstrcpy(dst, src);
    return (uint32_t)dst;
}

static uint32_t k32_lstrcatA(win32_call_t* call) {
    char* dst = (char*)ARG(0);
    const char* src = (const char*)ARG(1);
    if (!dst || !src) return 0;
    kstrcat(dst, src);
    return (uint32_t)dst;
}

static uint32_t k32_lstrcmpA(win32_call_t* call) {
    return (uint32_t)kstrcmp((const char*)ARG(0), (const char*)ARG(1));
}

static uint32_t k32_lstrcmpiA(win32_call_t* call) {
    return (uint32_t)kstricmp((const char*)ARG(0), (const char*)ARG(1));
}

/* CompareStringA(locale, flags, s1, len1, s2, len2) - returns 1/2/3 for
 * less/equal/greater, which is not the C convention. */
static uint32_t k32_CompareStringA(win32_call_t* call) {
    const char* a = (const char*)ARG(2);
    const char* b = (const char*)ARG(4);
    if (!a || !b) return 0;
    int r = (ARG(1) & 0x1) ? kstricmp(a, b) : kstrcmp(a, b);
    return r < 0 ? 1u : (r == 0 ? 2u : 3u);
}

static uint32_t k32_OutputDebugStringA(win32_call_t* call) {
    const char* s = (const char*)ARG(0);
    if (s) {
        terminal_writestring_color("[dbg] ", VGA_COLOR_DARK_GREY);
        terminal_writestring(s);
        terminal_writestring("\n");
    }
    return 0;
}

/* RaiseException(code, flags, argCount, args) - with no SEH there is
 * nothing to unwind to, so this is where the program ends. */
static uint32_t k32_RaiseException(win32_call_t* call) {
    w32_report_hex("program raised an exception, code", ARG(0));
    w32_exit_process(ARG(0));
    return 0;
}

static uint32_t k32_GetSystemDirectoryA(win32_call_t* call) {
    char* buffer = (char*)ARG(0);
    uint32_t size = ARG(1);
    const char* dir = "C:\\Novaris\\System32";
    uint32_t n = kstrlen(dir);
    if (!buffer || size <= n) return n + 1;
    kmemcpy(buffer, dir, n + 1);
    return n;
}

void w32_kernel32_reset(void) {
    /* Both point into the per-process heap arena, which is torn down
     * between programs - holding on to them would hand the next program a
     * dangling pointer. */
    cmdline_ansi = 0;
    cmdline_wide = 0;

    for (uint32_t i = 0; i < MAX_TLS_SLOTS; i++) {
        tls_slots[i] = 0;
        tls_used[i] = 0;
    }
}

/* --- export tables ----------------------------------------------------- */

static const win32_export_t kernel32_exports[] = {
    /* console */
    WEXP("GetStdHandle", 1, k32_GetStdHandle),
    WEXP("SetStdHandle", 2, k32_SetStdHandle),
    WEXP("WriteFile", 5, k32_WriteFile),
    WEXP("WriteConsoleA", 5, k32_WriteConsoleA),
    WEXP("WriteConsoleW", 5, k32_WriteConsoleW),
    WEXP("ReadFile", 5, k32_ReadFile),
    WEXP("ReadConsoleA", 5, k32_ReadConsoleA),
    WEXP("ReadConsoleW", 5, k32_ReadConsoleA),
    WEXP("SetConsoleTextAttribute", 2, k32_SetConsoleTextAttribute),
    WEXP("GetConsoleMode", 2, k32_GetConsoleMode),
    WSTUB("SetConsoleMode", 2, 1),
    WEXP("GetConsoleScreenBufferInfo", 2, k32_GetConsoleScreenBufferInfo),
    WSTUB("SetConsoleTitleA", 1, 1),
    WSTUB("SetConsoleTitleW", 1, 1),
    WSTUB("SetConsoleCursorPosition", 2, 1),
    WSTUB("SetConsoleCtrlHandler", 2, 1),
    WSTUB("AllocConsole", 0, 1),
    WSTUB("FreeConsole", 0, 1),
    WSTUB("AttachConsole", 1, 1),
    WSTUB("GetConsoleCP", 0, 437),
    WSTUB("GetConsoleOutputCP", 0, 437),
    WSTUB("SetConsoleCP", 1, 1),
    WSTUB("SetConsoleOutputCP", 1, 1),
    WSTUB("GetConsoleWindow", 0, 0),
    WSTUB("FlushConsoleInputBuffer", 1, 1),
    WSTUB("GetNumberOfConsoleInputEvents", 2, 0),
    WSTUB("FillConsoleOutputCharacterA", 5, 1),
    WSTUB("FillConsoleOutputAttribute", 5, 1),
    WSTUB("PeekConsoleInputA", 4, 0),
    WSTUB("ReadConsoleInputA", 4, 0),

    /* errors */
    WEXP("GetLastError", 0, k32_GetLastError),
    WEXP("SetLastError", 1, k32_SetLastError),
    WEXP("FormatMessageA", 7, k32_FormatMessageA),
    WEXP("FormatMessageW", 7, k32_FormatMessageA),

    /* process & modules */
    WEXP("ExitProcess", 1, k32_ExitProcess),
    WEXP("TerminateProcess", 2, k32_TerminateProcess),
    WEXP("GetCurrentProcess", 0, k32_GetCurrentProcess),
    WEXP("GetCurrentThread", 0, k32_GetCurrentThread),
    WEXP("GetCurrentProcessId", 0, k32_GetCurrentProcessId),
    WEXP("GetCurrentThreadId", 0, k32_GetCurrentThreadId),
    WEXP("GetModuleHandleA", 1, k32_GetModuleHandleA),
    WEXP("GetModuleHandleW", 1, k32_GetModuleHandleW),
    WEXP("GetModuleHandleExA", 3, k32_GetModuleHandleExA),
    WEXP("GetModuleHandleExW", 3, k32_GetModuleHandleExA),
    WEXP("GetModuleFileNameA", 3, k32_GetModuleFileNameA),
    WEXP("GetModuleFileNameW", 3, k32_GetModuleFileNameW),
    WEXP("GetProcAddress", 2, k32_GetProcAddress),
    WEXP("LoadLibraryA", 1, k32_LoadLibraryA),
    WEXP("LoadLibraryW", 1, k32_LoadLibraryW),
    WEXP("LoadLibraryExA", 3, k32_LoadLibraryExA),
    WEXP("LoadLibraryExW", 3, k32_LoadLibraryExA),
    WEXP("FreeLibrary", 1, k32_FreeLibrary),
    WEXP("GetCommandLineA", 0, k32_GetCommandLineA),
    WEXP("GetCommandLineW", 0, k32_GetCommandLineW),
    WEXP("GetStartupInfoA", 1, k32_GetStartupInfoA),
    WEXP("GetStartupInfoW", 1, k32_GetStartupInfoA),
    WEXP("GetEnvironmentStrings", 0, k32_GetEnvironmentStrings),
    WEXP("GetEnvironmentStringsA", 0, k32_GetEnvironmentStrings),
    WEXP("GetEnvironmentStringsW", 0, k32_GetEnvironmentStringsW),
    WEXP("FreeEnvironmentStringsA", 1, k32_FreeEnvironmentStrings),
    WEXP("FreeEnvironmentStringsW", 1, k32_FreeEnvironmentStrings),
    WEXP("GetEnvironmentVariableA", 3, k32_GetEnvironmentVariableA),
    WSTUB("GetEnvironmentVariableW", 3, 0),
    WSTUB("SetEnvironmentVariableA", 2, 1),
    WEXP("Sleep", 1, k32_Sleep),
    WSTUB("SleepEx", 2, 0),
    WEXP("SetUnhandledExceptionFilter", 1, k32_SetUnhandledExceptionFilter),
    WSTUB("UnhandledExceptionFilter", 1, 1),
    WSTUB("IsDebuggerPresent", 0, 0),
    WEXP("OutputDebugStringA", 1, k32_OutputDebugStringA),
    WSTUB("OutputDebugStringW", 1, 0),
    WSTUB("DebugBreak", 0, 0),
    WEXP("RaiseException", 4, k32_RaiseException),
    WSTUB("SetErrorMode", 1, 0),
    WSTUB("GetErrorMode", 0, 0),
    WEXP("IsProcessorFeaturePresent", 1, k32_IsProcessorFeaturePresent),
    WEXP("GetSystemInfo", 1, k32_GetSystemInfo),
    WEXP("GetNativeSystemInfo", 1, k32_GetSystemInfo),
    WEXP("GetVersion", 0, k32_GetVersion),
    WEXP("GetVersionExA", 1, k32_GetVersionExA),
    WEXP("GetVersionExW", 1, k32_GetVersionExA),
    WSTUB("GetProcessAffinityMask", 3, 1),
    WSTUB("SetPriorityClass", 2, 1),
    WSTUB("WaitForSingleObject", 2, 0),
    WSTUB("CreateThread", 6, 0),
    WSTUB("ExitThread", 1, 0),
    WSTUB("GetExitCodeProcess", 2, 1),
    WSTUB("CreateProcessA", 10, 0),
    WSTUB("SetThreadStackGuarantee", 1, 1),
    WSTUB("GetComputerNameA", 2, 0),
    WEXP("GetSystemDirectoryA", 2, k32_GetSystemDirectoryA),
    WEXP("GetWindowsDirectoryA", 2, k32_GetSystemDirectoryA),

    /* heap and memory */
    WEXP("GetProcessHeap", 0, k32_GetProcessHeap),
    WEXP("HeapAlloc", 3, k32_HeapAlloc),
    WEXP("HeapReAlloc", 4, k32_HeapReAlloc),
    WEXP("HeapFree", 3, k32_HeapFree),
    WEXP("HeapSize", 3, k32_HeapSize),
    WEXP("HeapCreate", 3, k32_HeapCreate),
    WSTUB("HeapDestroy", 1, 1),
    WSTUB("HeapValidate", 3, 1),
    WSTUB("HeapSetInformation", 4, 1),
    WSTUB("HeapCompact", 2, 0),
    WEXP("LocalAlloc", 2, k32_LocalAlloc),
    WEXP("LocalFree", 1, k32_LocalFree),
    WEXP("LocalReAlloc", 3, k32_LocalReAlloc),
    WEXP("LocalLock", 1, k32_Identity),
    WSTUB("LocalUnlock", 1, 1),
    WEXP("GlobalAlloc", 2, k32_LocalAlloc),
    WEXP("GlobalFree", 1, k32_LocalFree),
    WEXP("GlobalReAlloc", 3, k32_LocalReAlloc),
    WEXP("GlobalLock", 1, k32_Identity),
    WSTUB("GlobalUnlock", 1, 1),
    WEXP("GlobalMemoryStatus", 1, k32_GlobalMemoryStatus),
    WEXP("VirtualAlloc", 4, k32_VirtualAlloc),
    WEXP("VirtualFree", 3, k32_VirtualFree),
    WEXP("VirtualProtect", 4, k32_VirtualProtect),
    WEXP("VirtualQuery", 3, k32_VirtualQuery),
    WEXP("IsBadReadPtr", 2, k32_IsBadPtr),
    WEXP("IsBadWritePtr", 2, k32_IsBadPtr),
    WEXP("IsBadCodePtr", 1, k32_IsBadPtr),
    WEXP("EncodePointer", 1, k32_Identity),
    WEXP("DecodePointer", 1, k32_Identity),

    /* synchronization */
    WEXP("InitializeCriticalSection", 1, k32_InitializeCriticalSection),
    WEXP("InitializeCriticalSectionAndSpinCount", 2,
         k32_InitializeCriticalSectionAndSpinCount),
    WSTUB("InitializeCriticalSectionEx", 3, 1),
    WEXP("EnterCriticalSection", 1, k32_CriticalSectionNop),
    WEXP("LeaveCriticalSection", 1, k32_CriticalSectionNop),
    WEXP("DeleteCriticalSection", 1, k32_CriticalSectionNop),
    WEXP("TryEnterCriticalSection", 1, k32_TryEnterCriticalSection),
    WSTUB("InitializeSListHead", 1, 0),
    WSTUB("InitializeSRWLock", 1, 0),
    WSTUB("AcquireSRWLockExclusive", 1, 0),
    WSTUB("ReleaseSRWLockExclusive", 1, 0),
    WEXP("InitOnceExecuteOnce", 4, k32_InitOnceExecuteOnce),
    WEXP("TlsAlloc", 0, k32_TlsAlloc),
    WEXP("TlsGetValue", 1, k32_TlsGetValue),
    WEXP("TlsSetValue", 2, k32_TlsSetValue),
    WEXP("TlsFree", 1, k32_TlsFree),
    WEXP("InterlockedIncrement", 1, k32_InterlockedIncrement),
    WEXP("InterlockedDecrement", 1, k32_InterlockedDecrement),
    WEXP("InterlockedExchange", 2, k32_InterlockedExchange),
    WEXP("InterlockedExchangeAdd", 2, k32_InterlockedExchangeAdd),
    WEXP("InterlockedCompareExchange", 3, k32_InterlockedCompareExchange),
    WSTUB("CreateEventA", 4, 0),
    WSTUB("SetEvent", 1, 1),
    WSTUB("ResetEvent", 1, 1),
    WSTUB("CreateMutexA", 3, 0),
    WSTUB("ReleaseMutex", 1, 1),

    /* time */
    WEXP("GetTickCount", 0, k32_GetTickCount),
    WEXP("GetTickCount64", 0, k32_GetTickCount),
    WEXP("QueryPerformanceCounter", 1, k32_QueryPerformanceCounter),
    WEXP("QueryPerformanceFrequency", 1, k32_QueryPerformanceFrequency),
    WEXP("GetSystemTime", 1, k32_GetSystemTime),
    WEXP("GetLocalTime", 1, k32_GetSystemTime),
    WEXP("GetSystemTimeAsFileTime", 1, k32_GetSystemTimeAsFileTime),
    WEXP("GetTimeZoneInformation", 1, k32_GetTimeZoneInformation),
    WSTUB("SystemTimeToFileTime", 2, 1),
    WSTUB("FileTimeToSystemTime", 2, 1),
    WSTUB("FileTimeToLocalFileTime", 2, 1),
    WSTUB("GetDateFormatA", 6, 0),
    WSTUB("GetTimeFormatA", 6, 0),

    /* files */
    WEXP("CreateFileA", 7, k32_CreateFileA),
    WEXP("CreateFileW", 7, k32_CreateFileW),
    WEXP("CloseHandle", 1, k32_CloseHandle),
    WEXP("GetFileSize", 2, k32_GetFileSize),
    WEXP("SetFilePointer", 4, k32_SetFilePointer),
    WEXP("GetFileType", 1, k32_GetFileType),
    WEXP("GetFileAttributesA", 1, k32_GetFileAttributesA),
    WSTUB("GetFileAttributesW", 1, 0xFFFFFFFFu),
    WSTUB("FlushFileBuffers", 1, 1),
    WSTUB("SetEndOfFile", 1, 0),
    WSTUB("DeleteFileA", 1, 0),
    WSTUB("CreateDirectoryA", 2, 0),
    WEXP("FindFirstFileA", 2, k32_FindFirstFileA),
    WEXP("FindNextFileA", 2, k32_FindNextFileA),
    WEXP("FindClose", 1, k32_FindClose),
    WEXP("GetCurrentDirectoryA", 2, k32_GetCurrentDirectoryA),
    WSTUB("SetCurrentDirectoryA", 1, 0),
    WSTUB("GetFullPathNameA", 4, 0),
    WSTUB("GetTempPathA", 2, 0),
    WSTUB("SetHandleCount", 1, 0x100),
    WSTUB("CreateFileMappingA", 6, 0),
    WSTUB("MapViewOfFile", 5, 0),
    WSTUB("UnmapViewOfFile", 1, 1),

    /* character sets and locales */
    WEXP("MultiByteToWideChar", 6, k32_MultiByteToWideChar),
    WEXP("WideCharToMultiByte", 8, k32_WideCharToMultiByte),
    WEXP("GetACP", 0, k32_GetACP),
    WEXP("GetOEMCP", 0, k32_GetACP),
    WEXP("GetCPInfo", 2, k32_GetCPInfo),
    WEXP("IsValidCodePage", 1, k32_IsValidCodePage),
    WSTUB("IsDBCSLeadByte", 1, 0),    /* single-byte code page: never */
    WSTUB("IsDBCSLeadByteEx", 2, 0),
    WEXP("GetStringTypeW", 4, k32_GetStringTypeW),
    WSTUB("GetStringTypeA", 5, 1),
    WEXP("LCMapStringA", 6, k32_LCMapStringA),
    WEXP("LCMapStringW", 6, k32_LCMapStringA),
    WSTUB("GetUserDefaultLCID", 0, 0x0409),
    WSTUB("GetThreadLocale", 0, 0x0409),
    WSTUB("SetThreadLocale", 1, 0x0409),
    WSTUB("GetLocaleInfoA", 4, 0),
    WSTUB("GetLocaleInfoW", 4, 0),
    WSTUB("IsValidLocale", 2, 1),
    WSTUB("EnumSystemLocalesA", 2, 1),
    WEXP("lstrlenA", 1, k32_lstrlenA),
    WEXP("lstrlenW", 1, k32_lstrlenW),
    WEXP("lstrcpyA", 2, k32_lstrcpyA),
    WEXP("lstrcatA", 2, k32_lstrcatA),
    WEXP("lstrcmpA", 2, k32_lstrcmpA),
    WEXP("lstrcmpiA", 2, k32_lstrcmpiA),
    WEXP("CompareStringA", 6, k32_CompareStringA),
    WEXP("CompareStringW", 6, k32_CompareStringA),
    WSTUB("Beep", 2, 1),
};

const win32_module_t win32_module_kernel32 = {
    "kernel32.dll", kernel32_exports,
    sizeof(kernel32_exports) / sizeof(kernel32_exports[0]), 0
};

static uint32_t nt_NtCurrentTeb(win32_call_t* call) {
    (void)call;
    return w32_teb_address();
}

/* RtlAllocateHeap(heap, flags, size) - the flags match HeapAlloc's. */
static uint32_t nt_RtlAllocateHeap(win32_call_t* call) {
    return (ARG(1) & 0x8) ? w32_mem_alloc_zeroed(ARG(2)) : w32_mem_alloc(ARG(2));
}

static uint32_t nt_RtlFreeHeap(win32_call_t* call) {
    w32_mem_free(ARG(2));
    return 1;
}

/* ntdll: mingw's exception machinery and a few CRT internals import from
 * here. RtlUnwind is the one that matters - the CRT calls it while
 * tearing down after an exception, and it must not be a missing import
 * even though there's nothing for it to unwind. Symbols that ntdll and
 * the C runtime both export (memcpy, strlen, _vsnprintf) are left out
 * deliberately: win32.c's resolver falls back to msvcrt for them, so
 * they bind to one shared implementation rather than two. */
static const win32_export_t ntdll_exports[] = {
    WSTUB("RtlUnwind", 4, 0),
    WSTUB("RtlCaptureContext", 1, 0),
    WSTUB("RtlAddFunctionTable", 3, 1),
    WSTUB("RtlLookupFunctionEntry", 3, 0),
    WSTUB("RtlVirtualUnwind", 8, 0),
    WEXP("NtCurrentTeb", 0, nt_NtCurrentTeb),
    WEXP("RtlGetLastWin32Error", 0, k32_GetLastError),
    WSTUB("NtQueryInformationProcess", 5, 0xC0000001u),
    WSTUB("NtTerminateProcess", 2, 0),
    WEXP("RtlAllocateHeap", 3, nt_RtlAllocateHeap),
    WEXP("RtlFreeHeap", 3, nt_RtlFreeHeap),
    WSTUB("RtlSizeHeap", 3, 0),
    WSTUB("RtlEnterCriticalSection", 1, 0),
    WSTUB("RtlLeaveCriticalSection", 1, 0),
};

const win32_module_t win32_module_ntdll = {
    "ntdll.dll", ntdll_exports,
    sizeof(ntdll_exports) / sizeof(ntdll_exports[0]), 0
};
