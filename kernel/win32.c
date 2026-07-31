/* win32.c - the core of the Win32 emulation layer: the thunk arena, the
 * int 0x81 dispatcher, the memory arenas a Win32 program sees, and the
 * process setup/teardown around a running .exe.
 *
 * Read the long comment at the top of include/win32.h first - it explains
 * the thunk mechanism this file implements. The APIs themselves live in
 * win32_kernel32.c, win32_msvcrt.c and win32_user32.c. */

#include "win32_internal.h"
#include "win32_callback.h"
#include "paging.h"
#include "pmm.h"
#include "kheap.h"
#include "kstring.h"
#include "console.h"
#include "keyboard.h"
#include "process.h"
#include "gdt.h"
#include "pe.h"
#include "scheduler.h"

#define PAGE_SIZE 4096u

/* The running program's PEB. Every thread's TEB points at the same one -
 * a PEB is per *process* - so it outlives any single thread and has to be
 * reachable from w32_thread_create(). 0 outside a program. */
static uint32_t current_peb = 0;

/* --- module registry --------------------------------------------------- */

extern const win32_module_t win32_module_kernel32;
extern const win32_module_t win32_module_ntdll;
extern const win32_module_t win32_module_msvcrt;
extern const win32_module_t win32_module_user32;
extern const win32_module_t win32_module_gdi32;
extern const win32_module_t win32_module_advapi32;
extern const win32_module_t win32_module_shell32;
extern const win32_module_t win32_module_misc;

/* Aliases exist because the same implementations have to answer to many
 * DLL names. The UCRT in particular splits the C runtime across a dozen
 * api-ms-win-crt-*.dll files, and mingw-w64 links against whichever ones
 * a given function lives in. */
#define ALIAS(from, to) { from, 0, 0, to }

const win32_module_t win32_modules[] = {
    { "kernel32.dll", 0, 0, 0 }, /* filled in by win32_init from the externs */
    { "ntdll.dll",    0, 0, 0 },
    { "msvcrt.dll",   0, 0, 0 },
    { "user32.dll",   0, 0, 0 },
    { "gdi32.dll",    0, 0, 0 },
    { "advapi32.dll", 0, 0, 0 },
    { "shell32.dll",  0, 0, 0 },
    { "misc",         0, 0, 0 },

    ALIAS("ucrtbase.dll",                            "msvcrt.dll"),
    ALIAS("msvcrt40.dll",                            "msvcrt.dll"),
    ALIAS("msvcr70.dll",                             "msvcrt.dll"),
    ALIAS("msvcr71.dll",                             "msvcrt.dll"),
    ALIAS("msvcr80.dll",                             "msvcrt.dll"),
    ALIAS("msvcr90.dll",                             "msvcrt.dll"),
    ALIAS("msvcr100.dll",                            "msvcrt.dll"),
    ALIAS("msvcr110.dll",                            "msvcrt.dll"),
    ALIAS("msvcr120.dll",                            "msvcrt.dll"),
    ALIAS("api-ms-win-crt-runtime-l1-1-0.dll",       "msvcrt.dll"),
    ALIAS("api-ms-win-crt-stdio-l1-1-0.dll",         "msvcrt.dll"),
    ALIAS("api-ms-win-crt-heap-l1-1-0.dll",          "msvcrt.dll"),
    ALIAS("api-ms-win-crt-string-l1-1-0.dll",        "msvcrt.dll"),
    ALIAS("api-ms-win-crt-convert-l1-1-0.dll",       "msvcrt.dll"),
    ALIAS("api-ms-win-crt-math-l1-1-0.dll",          "msvcrt.dll"),
    ALIAS("api-ms-win-crt-locale-l1-1-0.dll",        "msvcrt.dll"),
    ALIAS("api-ms-win-crt-time-l1-1-0.dll",          "msvcrt.dll"),
    ALIAS("api-ms-win-crt-environment-l1-1-0.dll",   "msvcrt.dll"),
    ALIAS("api-ms-win-crt-filesystem-l1-1-0.dll",    "msvcrt.dll"),
    ALIAS("api-ms-win-crt-utility-l1-1-0.dll",       "msvcrt.dll"),
    ALIAS("api-ms-win-crt-process-l1-1-0.dll",       "msvcrt.dll"),
    ALIAS("api-ms-win-crt-conio-l1-1-0.dll",         "msvcrt.dll"),
    ALIAS("api-ms-win-core-console-l1-1-0.dll",      "kernel32.dll"),
    ALIAS("api-ms-win-core-processthreads-l1-1-0.dll", "kernel32.dll"),
    ALIAS("api-ms-win-core-file-l1-1-0.dll",         "kernel32.dll"),
    ALIAS("api-ms-win-core-heap-l1-1-0.dll",         "kernel32.dll"),
    ALIAS("api-ms-win-core-memory-l1-1-0.dll",       "kernel32.dll"),
    ALIAS("api-ms-win-core-libraryloader-l1-1-0.dll","kernel32.dll"),
    ALIAS("api-ms-win-core-synch-l1-1-0.dll",        "kernel32.dll"),
    ALIAS("api-ms-win-core-errorhandling-l1-1-0.dll","kernel32.dll"),
    ALIAS("api-ms-win-core-sysinfo-l1-1-0.dll",      "kernel32.dll"),
    ALIAS("kernelbase.dll",                          "kernel32.dll"),
};

const uint32_t win32_module_table_count =
    sizeof(win32_modules) / sizeof(win32_modules[0]);

/* The first N entries above are real tables whose exports/count fields
 * get patched in at init - a static initializer can't reference another
 * translation unit's `const` aggregate members portably. */
#define REAL_MODULE_COUNT 8
static win32_module_t modules[sizeof(win32_modules) / sizeof(win32_modules[0])];
static uint32_t module_count = 0;

/* Fake in-memory PE header per module, so GetModuleHandleA returns
 * something a program can dereference. */
static uint32_t module_handles[sizeof(win32_modules) / sizeof(win32_modules[0])];

/* --- slots ------------------------------------------------------------- */

/* One slot per exported symbol, flattened across every module. The
 * thunk's `mov eax, <index>` carries this index into the dispatcher. */
typedef struct {
    const win32_module_t* module;
    const win32_export_t* exp;
    uint32_t address; /* thunk address, or data address for data exports */
} win32_slot_t;

static win32_slot_t* slots = 0;
static uint32_t slot_count = 0;

#define THUNK_STRIDE 16u

/* Stubs for imports we have no export for at all. These get their own id
 * space (top bit set) because they aren't backed by a table entry. */
#define MISSING_ID_FLAG 0x80000000u
#define MAX_MISSING 192

typedef struct {
    char dll[24];
    char name[56];
    uint16_t ordinal;
    uint32_t address;
    uint32_t hits;
} win32_missing_t;

static win32_missing_t missing[MAX_MISSING];
static uint32_t missing_count = 0;

/* --- arenas ------------------------------------------------------------ */

static uint32_t thunk_next = WIN32_THUNK_BASE;
static uint32_t thunk_mapped_end = WIN32_THUNK_BASE;
static uint32_t data_next = WIN32_DATA_BASE;
static uint32_t data_mapped_end = WIN32_DATA_BASE;

/* Where boot-time allocation in the data arena stopped: the fake module
 * headers and the data exports below this mark belong to the emulation
 * layer itself and last forever, everything above it belongs to whichever
 * program is running (its TEB and PEB) and does not.
 *
 * Without this the arena only ever grew. Every program cost it another
 * TEB page plus a PEB, nothing ever gave them back, and after roughly a
 * hundred and twenty runs w32_data_alloc() would start returning 0 and
 * programs would stop starting. That has been true since Milestone 10 and
 * was found by watching `meminfo` lose exactly two frames per run across
 * the Milestone 15 smoke suite.
 *
 * The reset moves the bump pointer only; the pages stay mapped and get
 * reused by the next program, which is why it costs nothing to do. */
static uint32_t data_reset_mark = 0;

/* Maps arena pages on demand. Returns 0 if physical memory ran out. */
static int arena_grow(uint32_t* mapped_end, uint32_t want_end, uint32_t limit) {
    if (want_end > limit) return 0;
    while (*mapped_end < want_end) {
        uint32_t phys = pmm_alloc_frame();
        if (!phys) return 0;
        paging_map_page(*mapped_end, phys, PAGE_PRESENT | PAGE_RW | PAGE_USER);
        kmemset((void*)*mapped_end, 0, PAGE_SIZE);
        *mapped_end += PAGE_SIZE;
    }
    return 1;
}

uint32_t w32_data_alloc(uint32_t size, uint32_t align) {
    if (align < 4) align = 4;
    uint32_t addr = (data_next + align - 1) & ~(align - 1);
    uint32_t end = addr + size;
    if (!arena_grow(&data_mapped_end, (end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1),
                    WIN32_DATA_BASE + WIN32_DATA_SIZE)) {
        return 0;
    }
    data_next = end;
    return addr;
}

static uint32_t thunk_alloc(void) {
    uint32_t addr = thunk_next;
    if (!arena_grow(&thunk_mapped_end,
                    (addr + THUNK_STRIDE + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1),
                    WIN32_THUNK_BASE + WIN32_THUNK_SIZE)) {
        return 0;
    }
    thunk_next += THUNK_STRIDE;
    return addr;
}

/* Emits the four-instruction thunk described in win32.h.
 *
 *   B8 id      mov eax, id
 *   CD 81      int 0x81
 *   C2 nn 00   ret n      (stdcall: the callee pops the arguments)
 *   C3         ret        (cdecl: the caller does)
 *
 * The `ret` is what makes the calling convention come out right, and it
 * is the reason every export has to declare its argument count. */
static uint32_t emit_thunk(uint32_t id, int16_t argc) {
    uint32_t addr = thunk_alloc();
    if (!addr) return 0;

    uint8_t* p = (uint8_t*)addr;
    p[0] = 0xB8;
    p[1] = (uint8_t)(id & 0xFF);
    p[2] = (uint8_t)((id >> 8) & 0xFF);
    p[3] = (uint8_t)((id >> 16) & 0xFF);
    p[4] = (uint8_t)((id >> 24) & 0xFF);
    p[5] = 0xCD;
    p[6] = WIN32_INT_VECTOR;

    if (argc == WIN32_CDECL || argc <= 0) {
        p[7] = 0xC3;
    } else {
        uint16_t pop = (uint16_t)(argc * 4);
        p[7] = 0xC2;
        p[8] = (uint8_t)(pop & 0xFF);
        p[9] = (uint8_t)(pop >> 8);
    }
    return addr;
}

/* Copies a ring-3 native implementation into the arena, claiming as many
 * thunk slots as it needs (see WCODE in win32.h). */
static uint32_t emit_code(const uint8_t* code, uint16_t size) {
    uint32_t addr = thunk_alloc();
    if (!addr) return 0;

    uint32_t extra = (size + THUNK_STRIDE - 1) / THUNK_STRIDE;
    for (uint32_t i = 1; i < extra; i++) {
        if (!thunk_alloc()) return 0;
    }

    kmemcpy((void*)addr, code, size);
    return addr;
}

uint32_t w32_emit_ring3_code(const uint8_t* code, uint32_t size) {
    return emit_code(code, (uint16_t)size);
}

/* --- console ----------------------------------------------------------- */

static uint16_t console_attribute = 0x07; /* light grey on black */

void w32_set_attribute(uint16_t attribute) {
    /* Win32's FOREGROUND_ and BACKGROUND_ bits and the VGA attribute byte
     * are the same encoding (blue=1, green=2, red=4, intensity=8, with
     * the background in the high nibble), so no translation is needed. */
    console_attribute = attribute;
}

uint16_t w32_get_attribute(void) {
    return console_attribute;
}

void w32_write(const char* text, uint32_t length) {
    if (!text) return;
    terminal_setcolor((uint8_t)(console_attribute & 0xFF));
    for (uint32_t i = 0; i < length; i++) {
        char c = text[i];
        if (c == '\r') continue; /* CRLF collapses to the console's '\n' */
        if (c == '\t') {
            terminal_write("    ", 4);
            continue;
        }
        if (c == '\0') continue;
        terminal_putchar(c);
    }
    terminal_setcolor((uint8_t)VGA_COLOR_LIGHT_GREY);
}

void w32_write_str(const char* text) {
    if (text) w32_write(text, kstrlen(text));
}

void w32_report(const char* text) {
    terminal_writestring_color("[win32] ", VGA_COLOR_LIGHT_BROWN);
    terminal_writestring(text);
    terminal_writestring("\n");
}

void w32_report_hex(const char* label, uint32_t value) {
    char buf[16];
    terminal_writestring_color("[win32] ", VGA_COLOR_LIGHT_BROWN);
    terminal_writestring(label);
    terminal_writestring(" 0x");
    ku32_to_hex(value, buf, 0, 8);
    terminal_writestring(buf);
    terminal_writestring("\n");
}

void w32_report_dec(const char* label, uint32_t value) {
    char buf[16];
    terminal_writestring_color("[win32] ", VGA_COLOR_LIGHT_BROWN);
    terminal_writestring(label);
    terminal_writestring(" ");
    ku32_to_dec(value, buf);
    terminal_writestring(buf);
    terminal_writestring("\n");
}

uint32_t w32_read_line(char* buffer, uint32_t size) {
    if (size == 0) return 0;
    uint32_t len = 0;
    for (;;) {
        char c = keyboard_getchar();
        if (c == '\n') {
            terminal_putchar('\n');
            if (len + 1 < size) buffer[len++] = '\n';
            break;
        }
        if (c == '\b') {
            if (len > 0) { len--; terminal_backspace(); }
            continue;
        }
        if (len + 1 < size) {
            buffer[len++] = c;
            terminal_putchar(c);
        }
    }
    buffer[len] = '\0';
    return len;
}

/* --- last error -------------------------------------------------------- */

static uint32_t last_error = 0;
static uint32_t teb_address = 0;

void w32_set_error(uint32_t code) {
    last_error = code;
    /* The TEB's LastErrorValue field has to agree: some code reads it
     * directly through fs:[0x34] instead of calling GetLastError. */
    if (teb_address) *(uint32_t*)(teb_address + 0x34) = code;
}

uint32_t w32_get_error(void) {
    return last_error;
}

/* --- the heap a program sees ------------------------------------------- */

/* First-fit free list in the Win32 heap arena, same shape as the kernel's
 * own kheap but handing out *user*-accessible addresses. Whole-arena
 * teardown at process exit means fragmentation never has to be solved
 * properly here. */
typedef struct w32_block {
    uint32_t size;  /* usable bytes after this header */
    uint32_t used;
    uint32_t next;  /* address of the next header, 0 at the end */
} w32_block_t;

static uint32_t heap_head = 0;
static uint32_t heap_mapped_end = WIN32_HEAP_BASE;

static int heap_grow(uint32_t want_end) {
    return arena_grow(&heap_mapped_end,
                      (want_end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1),
                      WIN32_HEAP_BASE + WIN32_HEAP_SIZE);
}

static uint32_t heap_new_block(uint32_t size) {
    uint32_t addr = heap_mapped_end;
    uint32_t need = size + (uint32_t)sizeof(w32_block_t);
    if (!heap_grow(addr + need)) return 0;

    w32_block_t* b = (w32_block_t*)addr;
    b->size = heap_mapped_end - addr - (uint32_t)sizeof(w32_block_t);
    b->used = 1;
    b->next = 0;

    if (!heap_head) {
        heap_head = addr;
    } else {
        w32_block_t* tail = (w32_block_t*)heap_head;
        while (tail->next) tail = (w32_block_t*)tail->next;
        tail->next = addr;
    }
    return addr + (uint32_t)sizeof(w32_block_t);
}

uint32_t w32_mem_alloc(uint32_t size) {
    if (size == 0) size = 1;
    size = (size + 7u) & ~7u;

    for (uint32_t cur = heap_head; cur; cur = ((w32_block_t*)cur)->next) {
        w32_block_t* b = (w32_block_t*)cur;
        if (b->used || b->size < size) continue;

        if (b->size >= size + sizeof(w32_block_t) + 16) {
            uint32_t rest_addr = cur + (uint32_t)sizeof(w32_block_t) + size;
            w32_block_t* rest = (w32_block_t*)rest_addr;
            rest->size = b->size - size - (uint32_t)sizeof(w32_block_t);
            rest->used = 0;
            rest->next = b->next;
            b->size = size;
            b->next = rest_addr;
        }
        b->used = 1;
        return cur + (uint32_t)sizeof(w32_block_t);
    }
    return heap_new_block(size);
}

uint32_t w32_mem_alloc_zeroed(uint32_t size) {
    uint32_t p = w32_mem_alloc(size);
    if (p) kmemset((void*)p, 0, size);
    return p;
}

uint32_t w32_mem_size(uint32_t addr) {
    if (!addr) return 0;
    return ((w32_block_t*)(addr - sizeof(w32_block_t)))->size;
}

void w32_mem_free(uint32_t addr) {
    if (!addr) return;
    if (addr < WIN32_HEAP_BASE || addr >= heap_mapped_end) return;

    w32_block_t* b = (w32_block_t*)(addr - sizeof(w32_block_t));
    b->used = 0;

    /* Blocks are always in increasing address order, so one forward pass
     * merging adjacent free neighbours coalesces everything. */
    for (uint32_t cur = heap_head; cur;) {
        w32_block_t* c = (w32_block_t*)cur;
        if (!c->next) break;
        w32_block_t* n = (w32_block_t*)c->next;
        if (!c->used && !n->used &&
            c->next == cur + sizeof(w32_block_t) + c->size) {
            c->size += (uint32_t)sizeof(w32_block_t) + n->size;
            c->next = n->next;
        } else {
            cur = c->next;
        }
    }
}

uint32_t w32_mem_realloc(uint32_t addr, uint32_t size) {
    if (!addr) return w32_mem_alloc(size);
    if (size == 0) { w32_mem_free(addr); return 0; }

    uint32_t old = w32_mem_size(addr);
    if (old >= size) return addr;

    uint32_t fresh = w32_mem_alloc(size);
    if (!fresh) return 0;
    kmemcpy((void*)fresh, (void*)addr, old);
    w32_mem_free(addr);
    return fresh;
}

uint32_t w32_mem_alloc_pages(uint32_t size) {
    /* VirtualAlloc's callers expect page alignment, which the block
     * allocator can't promise - so this takes whole pages off the top of
     * the arena instead and never reuses them. Programs that VirtualAlloc
     * in a loop would leak; nothing this OS runs does. */
    uint32_t bytes = (size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if (bytes == 0) bytes = PAGE_SIZE;
    uint32_t addr = (heap_mapped_end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if (!heap_grow(addr + bytes)) return 0;
    kmemset((void*)addr, 0, bytes);
    return addr;
}

uint32_t w32_strdup_user(const char* s) {
    if (!s) return 0;
    uint32_t n = kstrlen(s) + 1;
    uint32_t p = w32_mem_alloc(n);
    if (p) kmemcpy((void*)p, s, n);
    return p;
}

void w32_mem_reset(void) {
    for (uint32_t va = WIN32_HEAP_BASE; va < heap_mapped_end; va += PAGE_SIZE) {
        uint32_t pte = paging_get_entry(va);
        paging_unmap_page(va);
        if (pte & PAGE_PRESENT) pmm_free_frame(pte & ~0xFFFu);
    }
    heap_mapped_end = WIN32_HEAP_BASE;
    heap_head = 0;
}

/* --- handles ----------------------------------------------------------- */

#define MAX_W32_HANDLES 32
static w32_handle_t handles[MAX_W32_HANDLES];

int w32_is_console_handle(uint32_t handle) {
    return handle == W32_STDIN_HANDLE || handle == W32_STDOUT_HANDLE ||
           handle == W32_STDERR_HANDLE;
}

uint32_t w32_handle_open(w32_handle_type_t type, void* node) {
    for (uint32_t i = 0; i < MAX_W32_HANDLES; i++) {
        if (handles[i].type == W32_H_FREE) {
            handles[i].type = type;
            handles[i].node = node;
            handles[i].position = 0;
            handles[i].index = 0;
            return 0x20000000u + i; /* tagged so it can't collide with a pointer */
        }
    }
    return W32_INVALID_HANDLE_VALUE;
}

w32_handle_t* w32_handle_get(uint32_t handle) {
    if (handle < 0x20000000u || handle >= 0x20000000u + MAX_W32_HANDLES) return 0;
    w32_handle_t* h = &handles[handle - 0x20000000u];
    return h->type == W32_H_FREE ? 0 : h;
}

void w32_handle_close(uint32_t handle) {
    w32_handle_t* h = w32_handle_get(handle);
    if (h) {
        h->type = W32_H_FREE;
        h->node = 0;
    }
}

void w32_handles_reset(void) {
    for (uint32_t i = 0; i < MAX_W32_HANDLES; i++) {
        handles[i].type = W32_H_FREE;
        handles[i].node = 0;
    }
}

/* --- strings ----------------------------------------------------------- */

uint32_t w32_wide_len(const uint16_t* s) {
    uint32_t n = 0;
    if (!s) return 0;
    while (s[n]) n++;
    return n;
}

uint32_t w32_wide_to_ascii(const uint16_t* src, char* dst, uint32_t dst_size) {
    if (!src || !dst || dst_size == 0) return 0;
    uint32_t n = 0;
    while (src[n] && n + 1 < dst_size) {
        dst[n] = src[n] < 128 ? (char)src[n] : '?';
        n++;
    }
    dst[n] = '\0';
    return n;
}

uint32_t w32_ascii_to_wide(const char* src, uint16_t* dst, uint32_t dst_chars) {
    if (!src || !dst || dst_chars == 0) return 0;
    uint32_t n = 0;
    while (src[n] && n + 1 < dst_chars) {
        dst[n] = (uint16_t)(uint8_t)src[n];
        n++;
    }
    dst[n] = 0;
    return n;
}

const char* w32_wide_arg(const uint16_t* s) {
    static char scratch[260]; /* MAX_PATH, which is what these are used for */
    if (!s) return 0;
    w32_wide_to_ascii(s, scratch, sizeof(scratch));
    return scratch;
}

/* --- the running program ----------------------------------------------- */

static pe_image_t current_image;
static int process_active = 0;
static char current_name[64];
static char current_cmdline[256];
static uint32_t exit_code = 0;
static uint32_t exception_filter = 0;
static uint32_t stack_pages_mapped = 0;
static int last_run_faulted = 0;

const pe_image_t* w32_current_image(void) {
    return process_active ? &current_image : 0;
}

const char* w32_current_name(void) { return current_name; }
const char* w32_current_cmdline(void) { return current_cmdline; }

int win32_process_active(void) { return process_active; }

int win32_last_run_faulted(void) { return last_run_faulted; }

void w32_set_exception_filter(uint32_t fn) { exception_filter = fn; }
uint32_t w32_get_exception_filter(void) { return exception_filter; }

void w32_exit_process(uint32_t code) {
    exit_code = code;

    /* Anything the program registered with atexit() runs here, in ring 3,
     * before the unwind below throws away the stack it would need. This
     * is the one place every exit path converges on - exit(), abort(),
     * ExitProcess(), and `return` from main via the exit trampoline - so
     * putting it here means handlers run for all of them.
     * w32_msvcrt_run_atexit() is a no-op if there are none, and refuses
     * to re-enter if a handler calls exit() itself. */
    w32_msvcrt_run_atexit();

    /* Every thread goes with it: ExitProcess means the process. The
     * scheduler unwinds to win32_run_pe()'s scheduler_run_until_idle()
     * call, which is where teardown lives. */
    if (scheduler_is_active()) {
        scheduler_terminate_all();
        __builtin_unreachable();
    }

    /* Unwinds straight back to win32_run_pe's frame, discarding the
     * interrupt/dispatch call chain we're standing on - the same trick
     * process.c already uses for sys_exit. Teardown happens there, on the
     * kernel's own stack, not here. */
    process_exit_to_kernel();
    __builtin_unreachable();
}

/* --- dispatch ---------------------------------------------------------- */

/* The ring-3 esp of the innermost emulated call in flight, 0 outside one.
 * See where win32_dispatch() sets it. */
static uint32_t dispatch_user_esp = 0;

uint32_t w32_current_user_esp(void) {
    return dispatch_user_esp;
}

static void report_missing_call(uint32_t index, registers_t* regs) {
    win32_missing_t* m = &missing[index];
    m->hits++;
    if (m->hits > 1) return; /* one report per symbol, however often it's called */

    terminal_writestring_color("[win32] ", VGA_COLOR_LIGHT_BROWN);
    terminal_writestring("unimplemented API called: ");
    terminal_writestring(m->dll);
    terminal_writestring("!");
    if (m->name[0]) {
        terminal_writestring(m->name);
    } else {
        char buf[12];
        terminal_writestring("#");
        ku32_to_dec(m->ordinal, buf);
        terminal_writestring(buf);
    }
    terminal_writestring(" -> returning 0\n");
    terminal_writestring_color("        ", VGA_COLOR_LIGHT_BROWN);
    terminal_writestring("(its argument count is unknown, so the stack may be\n");
    terminal_writestring("         left unbalanced - see ROADMAP.md Milestone 10)\n");
    (void)regs;
}

static void win32_dispatch(registers_t* regs) {
    /* Only ring 3 has a meaningful useresp in the trap frame; a kernel
     * `int 0x81` would make us read the wrong stack entirely. */
    if ((regs->cs & 3) != 3) {
        w32_report("int 0x81 from ring 0 - ignored");
        regs->eax = 0;
        return;
    }

    uint32_t id = regs->eax;

    win32_call_t call;
    call.regs = regs;
    /* Snapshot the ring-3 esp before anything this call does can lead to
     * a nested trap - see the comment on win32_call_t.useresp. */
    call.useresp = regs->useresp;
    /* [useresp] is the thunk's return address; the arguments follow it. */
    call.args = (const uint32_t*)(call.useresp + 4);

    /* Published for the handful of places that need the caller's stack
     * but don't have the win32_call_t to hand - w32_exit_process() running
     * atexit handlers, mainly. Saved and restored so a nested dispatch
     * (an API called from inside a ring-3 callback) doesn't leave the
     * outer call looking at the wrong stack. */
    uint32_t prev_user_esp = dispatch_user_esp;
    dispatch_user_esp = call.useresp;

    if (id & MISSING_ID_FLAG) {
        uint32_t index = id & ~MISSING_ID_FLAG;
        if (index < missing_count) report_missing_call(index, regs);
        regs->eax = 0;
    } else if (id >= slot_count) {
        w32_report_hex("dispatch called with an unknown slot id", id);
        regs->eax = 0;
    } else {
        const win32_export_t* e = slots[id].exp;
        if (e->fn) {
            regs->eax = e->fn(&call);
        } else {
            regs->eax = e->stub_ret;
        }
    }

    dispatch_user_esp = prev_user_esp;
}

/* --- resolution -------------------------------------------------------- */

static int module_index(const char* name) {
    for (uint32_t i = 0; i < module_count; i++) {
        if (kstricmp(modules[i].name, name) == 0) {
            if (modules[i].alias_of) return module_index(modules[i].alias_of);
            return (int)i;
        }
    }
    return -1;
}

int w32_module_index(const char* name) {
    return module_index(name);
}

uint32_t w32_teb_address(void) {
    return teb_address;
}

static const win32_export_t* find_export(int mod, const char* name,
                                         uint16_t ordinal, uint32_t* out_addr) {
    if (mod < 0) return 0;
    for (uint32_t i = 0; i < slot_count; i++) {
        if (slots[i].module != &modules[mod]) continue;
        const win32_export_t* e = slots[i].exp;
        if (name) {
            if (kstrcmp(e->name, name) != 0) continue;
        } else {
            if (e->ordinal == 0 || e->ordinal != ordinal) continue;
        }
        if (out_addr) *out_addr = slots[i].address;
        return e;
    }
    return 0;
}

uint32_t w32_export_address(int module_index_, const char* name, uint16_t ordinal) {
    uint32_t addr = 0;
    if (find_export(module_index_, name, ordinal, &addr)) return addr;
    return 0;
}

int win32_have_export(const char* dll, const char* name, uint16_t ordinal) {
    return find_export(module_index(dll), name, ordinal, 0) != 0;
}

/* Resolution order: the named module first, then three fallbacks tried by
 * name only. This matters more than it looks. The same symbol genuinely
 * lives in different DLLs across toolchains and Windows versions -
 * ntdll exports memcpy and strlen, kernelbase took over half of
 * kernel32, `misc` collects the ones that turn up under DLL names this
 * OS doesn't model at all. Falling back means an import of
 * ntdll.dll!memcpy binds to the C runtime's memcpy rather than becoming
 * a missing-import warning. A real implementation in the named module
 * always wins, so the fallback can only ever add resolutions. */
static uint32_t resolve_symbol(const char* dll, const char* name,
                               uint16_t ordinal) {
    uint32_t addr = 0;
    int mod = module_index(dll);
    if (mod >= 0 && find_export(mod, name, ordinal, &addr)) return addr;

    if (!name) return 0; /* ordinals are only meaningful within one module */

    static const char* fallbacks[] = { "misc", "msvcrt.dll", "kernel32.dll" };
    for (uint32_t i = 0; i < sizeof(fallbacks) / sizeof(fallbacks[0]); i++) {
        int f = module_index(fallbacks[i]);
        if (f >= 0 && f != mod && find_export(f, name, 0, &addr)) return addr;
    }
    return 0;
}

static uint32_t loader_resolve(const char* dll, const char* name,
                               uint16_t ordinal, void* ctx) {
    (void)ctx;
    return resolve_symbol(dll, name, ordinal);
}

static uint32_t loader_missing(const char* dll, const char* name,
                               uint16_t ordinal, void* ctx) {
    (void)ctx;

    /* Reuse an existing stub if this same symbol was already missing -
     * one .exe can import the same function from two descriptors. */
    for (uint32_t i = 0; i < missing_count; i++) {
        if (kstricmp(missing[i].dll, dll) != 0) continue;
        if (name) {
            if (kstrcmp(missing[i].name, name) == 0) return missing[i].address;
        } else if (missing[i].ordinal == ordinal && missing[i].name[0] == '\0') {
            return missing[i].address;
        }
    }

    if (missing_count >= MAX_MISSING) return 0;

    win32_missing_t* m = &missing[missing_count];
    kstrlcpy(m->dll, dll, sizeof(m->dll));
    if (name) {
        kstrlcpy(m->name, name, sizeof(m->name));
    } else {
        m->name[0] = '\0';
    }
    m->ordinal = ordinal;
    m->hits = 0;
    /* Cdecl-form thunk: with no declaration to consult we don't know how
     * many arguments to pop, and popping the wrong number is worse than
     * popping none - see report_missing_call(). */
    m->address = emit_thunk(MISSING_ID_FLAG | missing_count, WIN32_CDECL);
    if (!m->address) return 0;

    missing_count++;
    return m->address;
}

/* --- fake module images ------------------------------------------------ */

/* A page per module holding just enough of a PE header that a program
 * poking at the result of GetModuleHandleA() finds what it expects
 * instead of faulting. */
static uint32_t build_fake_module(const char* name) {
    (void)name;
    uint32_t base = w32_data_alloc(PAGE_SIZE, PAGE_SIZE);
    if (!base) return 0;

    dos_header_t* dos = (dos_header_t*)base;
    dos->e_magic = IMAGE_DOS_SIGNATURE;
    dos->e_lfanew = 0x40;

    *(uint32_t*)(base + 0x40) = IMAGE_NT_SIGNATURE;
    coff_header_t* coff = (coff_header_t*)(base + 0x44);
    coff->Machine = IMAGE_FILE_MACHINE_I386;
    coff->NumberOfSections = 0;
    coff->SizeOfOptionalHeader = (uint16_t)sizeof(optional_header32_t);
    coff->Characteristics = IMAGE_FILE_DLL;

    optional_header32_t* opt =
        (optional_header32_t*)(base + 0x44 + sizeof(coff_header_t));
    opt->Magic = IMAGE_NT_OPTIONAL_HDR32_MAGIC;
    opt->ImageBase = base;
    opt->SizeOfImage = PAGE_SIZE;
    opt->SizeOfHeaders = 0x200;
    opt->Subsystem = IMAGE_SUBSYSTEM_WINDOWS_CUI;
    opt->MajorOperatingSystemVersion = 6;
    opt->MajorSubsystemVersion = 6;
    opt->NumberOfRvaAndSizes = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;

    return base;
}

uint32_t w32_module_handle(const char* name) {
    int idx = module_index(name);
    if (idx < 0) {
        /* Also accept a bare name with no extension ("kernel32"). */
        char with_ext[40];
        kstrlcpy(with_ext, name, sizeof(with_ext) - 5);
        kstrcat(with_ext, ".dll");
        idx = module_index(with_ext);
    }
    if (idx < 0) return 0;
    return module_handles[idx];
}

int w32_module_index_from_handle(uint32_t handle) {
    for (uint32_t i = 0; i < module_count; i++) {
        if (module_handles[i] && module_handles[i] == handle) return (int)i;
    }
    return -1;
}

/* --- enumeration for the shell ----------------------------------------- */

uint32_t win32_module_count(void) { return module_count; }

const char* win32_module_name(uint32_t index) {
    return index < module_count ? modules[index].name : 0;
}

uint32_t win32_module_exports(uint32_t index) {
    if (index >= module_count) return 0;
    if (modules[index].alias_of) return 0;
    return modules[index].count;
}

uint32_t win32_module_implemented(uint32_t index) {
    if (index >= module_count || modules[index].alias_of) return 0;
    uint32_t n = 0;
    for (uint32_t i = 0; i < modules[index].count; i++) {
        if (!(modules[index].exports[i].flags & WIN32_EXPORT_STUB)) n++;
    }
    return n;
}

void win32_list_exports(uint32_t index, int implemented_only) {
    if (index >= module_count || modules[index].alias_of) return;

    uint32_t column = 0;
    for (uint32_t i = 0; i < modules[index].count; i++) {
        const win32_export_t* e = &modules[index].exports[i];
        if (implemented_only && (e->flags & WIN32_EXPORT_STUB)) continue;

        terminal_writestring("  ");
        terminal_writestring(e->name);

        /* The marker counts toward the column width, or the columns drift
         * by one for every stub in the list. */
        uint32_t width = kstrlen(e->name);
        if (e->flags & WIN32_EXPORT_STUB) {
            terminal_writestring("*");
            width++;
        }
        if (e->data_size) {
            terminal_writestring("=");
            width++;
        }

        if (++column % 3 == 0) {
            terminal_writestring("\n");
        } else {
            /* Rough column alignment - names vary a lot in length and the
             * console has no tab stops. */
            for (uint32_t s = width; s < 24; s++) terminal_writestring(" ");
        }
    }
    if (column % 3 != 0) terminal_writestring("\n");
}

/* --- init -------------------------------------------------------------- */

void win32_init(void) {
    const win32_module_t* real[REAL_MODULE_COUNT] = {
        &win32_module_kernel32, &win32_module_ntdll, &win32_module_msvcrt,
        &win32_module_user32,   &win32_module_gdi32, &win32_module_advapi32,
        &win32_module_shell32,  &win32_module_misc,
    };

    module_count = win32_module_table_count;
    for (uint32_t i = 0; i < module_count; i++) {
        modules[i] = win32_modules[i];
        if (i < REAL_MODULE_COUNT) {
            modules[i].exports = real[i]->exports;
            modules[i].count = real[i]->count;
        }
    }

    /* The address ranges the emulation owns must be off-limits to the PE
     * loader when it picks a load address (see paging.h). */
    paging_reserve_region(WIN32_THUNK_BASE, WIN32_THUNK_BASE + WIN32_THUNK_SIZE,
                          "win32 thunks");
    paging_reserve_region(WIN32_DATA_BASE, WIN32_DATA_BASE + WIN32_DATA_SIZE,
                          "win32 data");
    paging_reserve_region(WIN32_HEAP_BASE, WIN32_HEAP_BASE + WIN32_HEAP_SIZE,
                          "win32 heap");
    paging_reserve_region(WIN32_STACK_TOP - WIN32_STACK_SIZE, WIN32_STACK_TOP,
                          "win32 stack");

    uint32_t total = 0;
    for (uint32_t i = 0; i < REAL_MODULE_COUNT; i++) total += modules[i].count;

    slots = (win32_slot_t*)kmalloc(total * sizeof(win32_slot_t));
    if (!slots) {
        w32_report("out of memory building the export table");
        return;
    }

    slot_count = 0;
    for (uint32_t m = 0; m < REAL_MODULE_COUNT; m++) {
        for (uint32_t e = 0; e < modules[m].count; e++) {
            const win32_export_t* exp = &modules[m].exports[e];
            slots[slot_count].module = &modules[m];
            slots[slot_count].exp = exp;

            if (exp->data_size) {
                /* A variable, not a function: the IAT gets the address of
                 * real storage that the program reads and writes. */
                slots[slot_count].address = w32_data_alloc(exp->data_size, 8);
            } else if (exp->code) {
                slots[slot_count].address = emit_code(exp->code, exp->code_size);
            } else {
                slots[slot_count].address = emit_thunk(slot_count, exp->argc);
            }
            slot_count++;
        }
    }

    for (uint32_t i = 0; i < module_count; i++) {
        module_handles[i] = modules[i].alias_of ? 0 : build_fake_module(modules[i].name);
    }
    /* Aliases share their target's handle, so GetModuleHandle("ucrtbase")
     * and GetModuleHandle("msvcrt") agree. */
    for (uint32_t i = 0; i < module_count; i++) {
        if (modules[i].alias_of) {
            int t = module_index(modules[i].alias_of);
            if (t >= 0) module_handles[i] = module_handles[t];
        }
    }

    register_interrupt_handler(WIN32_INT_VECTOR, win32_dispatch);

    /* Emits the ring-3 callback-return stub and claims int 0x82. Has to
     * come after the export thunks so it lands past them in the arena. */
    if (!win32_callback_init()) {
        w32_report("thunk arena full - qsort/bsearch/atexit callbacks disabled");
    }

    w32_handles_reset();

    /* The CRT's stdio table can only be filled in once its data export
     * has an address, which is why this runs last. */
    w32_msvcrt_init();

    /* Everything allocated in the data arena from here on belongs to a
     * running program and is reclaimed when it exits. */
    data_reset_mark = data_next;

    /* Put the x87 unit in a known state. Nothing in the kernel uses it,
     * but a couple of emulated CRT functions are ring-3 x87 code blobs
     * (see win32_msvcrt.c), and starting from a defined control word
     * beats inheriting whatever GRUB left behind. */
    __asm__ __volatile__("fninit");
}

/* --- process setup ----------------------------------------------------- */

/* Thread Environment Block. Only the fields a real program touches are
 * filled in, but those matter a great deal: mingw's C runtime installs an
 * SEH handler with `mov fs:[0], esp` before main() ever runs, so fs has
 * to point at writable memory whose first word is the exception list. */
static uint32_t build_teb(uint32_t peb, uint32_t stack_top, uint32_t stack_base) {
    uint32_t teb = w32_data_alloc(PAGE_SIZE, PAGE_SIZE);
    if (!teb) return 0;
    kmemset((void*)teb, 0, PAGE_SIZE);

    *(uint32_t*)(teb + 0x00) = 0xFFFFFFFFu; /* ExceptionList: end of chain */
    *(uint32_t*)(teb + 0x04) = stack_top;   /* StackBase (the high address) */
    *(uint32_t*)(teb + 0x08) = stack_base;  /* StackLimit (the low address) */
    *(uint32_t*)(teb + 0x18) = teb;         /* Self */
    *(uint32_t*)(teb + 0x20) = 0x100;       /* ClientId.UniqueProcess */
    *(uint32_t*)(teb + 0x24) = 0x104;       /* ClientId.UniqueThread */
    *(uint32_t*)(teb + 0x2C) = teb + 0x0E10;/* ThreadLocalStoragePointer */
    *(uint32_t*)(teb + 0x30) = peb;         /* ProcessEnvironmentBlock */
    *(uint32_t*)(teb + 0x34) = 0;           /* LastErrorValue */

    return teb;
}

static uint32_t build_peb(uint32_t image_base) {
    uint32_t peb = w32_data_alloc(0x260, 8);
    if (!peb) return 0;
    kmemset((void*)peb, 0, 0x260);

    *(uint32_t*)(peb + 0x08) = image_base;      /* ImageBaseAddress */
    *(uint32_t*)(peb + 0x18) = W32_HEAP_HANDLE; /* ProcessHeap */
    *(uint32_t*)(peb + 0xA4) = 6;               /* OSMajorVersion */
    *(uint32_t*)(peb + 0xA8) = 1;               /* OSMinorVersion  (7 == 6.1) */
    *(uint16_t*)(peb + 0xAC) = 7601;            /* OSBuildNumber */
    *(uint32_t*)(peb + 0xB0) = 2;               /* VER_PLATFORM_WIN32_NT */
    *(uint32_t*)(peb + 0xB4) = IMAGE_SUBSYSTEM_WINDOWS_CUI;

    return peb;
}

/* Emits the trampoline the entry point returns *to*: whatever a C main()
 * eventually returns ends up in eax, and this turns that into the
 * ExitProcess call the program never made itself.
 *
 *   50            push eax                 ; exit code becomes the argument
 *   B8 <thunk>    mov eax, ExitProcess thunk
 *   FF D0         call eax                 ; pushes the return address the
 *                                          ; thunk's `ret 4` will consume
 *   EB FE         jmp $                    ; unreachable; a guard rail in
 *                                          ; case ExitProcess ever returns
 */
static uint32_t build_exit_trampoline(void) {
    uint32_t exit_thunk = resolve_symbol("kernel32.dll", "ExitProcess", 0);
    if (!exit_thunk) return 0;

    uint32_t addr = thunk_alloc();
    if (!addr) return 0;

    uint8_t* p = (uint8_t*)addr;
    p[0] = 0x50;
    p[1] = 0xB8;
    *(uint32_t*)(p + 2) = exit_thunk;
    p[6] = 0xFF; p[7] = 0xD0;
    p[8] = 0xEB; p[9] = 0xFE;
    return addr;
}

/* The same shape, but for a thread: when a CreateThread start routine
 * returns, its return value is its exit code and it must end only that
 * thread, not the program. Identical bytes to build_exit_trampoline()
 * except for which thunk it calls. */
static uint32_t build_thread_exit_trampoline(void) {
    uint32_t exit_thunk = resolve_symbol("kernel32.dll", "ExitThread", 0);
    if (!exit_thunk) return 0;

    uint32_t addr = thunk_alloc();
    if (!addr) return 0;

    uint8_t* p = (uint8_t*)addr;
    p[0] = 0x50;                        /* push eax - the return value */
    p[1] = 0xB8;
    *(uint32_t*)(p + 2) = exit_thunk;   /* mov eax, ExitThread thunk */
    p[6] = 0xFF; p[7] = 0xD0;           /* call eax */
    p[8] = 0xEB; p[9] = 0xFE;           /* jmp $ - ExitThread never returns */
    return addr;
}

/* Emits a trampoline that runs the image's TLS callbacks (each stdcall
 * with (base, DLL_PROCESS_ATTACH, NULL)) and then jumps to the real entry
 * point. Only used when the image actually has callbacks. Byte layout is
 * spelled out because the two short jumps are hand-computed:
 *
 *   00  BE <array>   mov esi, callback array
 *   05  AD           lodsd                  ; eax = *esi++
 *   06  85 C0        test eax, eax
 *   08  74 0D        jz +13 -> 0x17
 *   0A  6A 00        push 0                 ; Reserved
 *   0C  6A 01        push 1                 ; DLL_PROCESS_ATTACH
 *   0E  68 <base>    push image base
 *   13  FF D0        call eax               ; stdcall: the callee pops
 *   15  EB EE        jmp -18 -> 0x05
 *   17  B8 <entry>   mov eax, entry point
 *   1C  FF E0        jmp eax
 */
static uint32_t build_tls_trampoline(uint32_t callbacks, uint32_t base,
                                     uint32_t entry) {
    uint32_t addr = thunk_alloc();
    if (!addr) return 0;
    /* 30 bytes - two thunk slots' worth. */
    if (!thunk_alloc()) return 0;

    uint8_t* p = (uint8_t*)addr;
    p[0] = 0xBE; *(uint32_t*)(p + 1) = callbacks;
    p[5] = 0xAD;
    p[6] = 0x85; p[7] = 0xC0;
    p[8] = 0x74; p[9] = 0x0D;
    p[10] = 0x6A; p[11] = 0x00;
    p[12] = 0x6A; p[13] = 0x01;
    p[14] = 0x68; *(uint32_t*)(p + 15) = base;
    p[19] = 0xFF; p[20] = 0xD0;
    p[21] = 0xEB; p[22] = 0xEE;
    p[23] = 0xB8; *(uint32_t*)(p + 24) = entry;
    p[28] = 0xFF; p[29] = 0xE0;
    return addr;
}

static int map_stack(void) {
    uint32_t base = WIN32_STACK_TOP - WIN32_STACK_SIZE;
    stack_pages_mapped = 0;
    for (uint32_t va = base; va < WIN32_STACK_TOP; va += PAGE_SIZE) {
        uint32_t phys = pmm_alloc_frame();
        if (!phys) return 0;
        paging_map_page(va, phys, PAGE_PRESENT | PAGE_RW | PAGE_USER);
        stack_pages_mapped++;
    }
    kmemset((void*)base, 0, WIN32_STACK_SIZE);
    return 1;
}

static void unmap_stack(void) {
    uint32_t base = WIN32_STACK_TOP - WIN32_STACK_SIZE;
    for (uint32_t i = 0; i < stack_pages_mapped; i++) {
        uint32_t va = base + i * PAGE_SIZE;
        uint32_t pte = paging_get_entry(va);
        paging_unmap_page(va);
        if (pte & PAGE_PRESENT) pmm_free_frame(pte & ~0xFFFu);
    }
    stack_pages_mapped = 0;
}

/* --- threads ------------------------------------------------------------
 *
 * A Win32 program is one or more scheduler tasks in one address space -
 * which is what Milestone 16 built. Everything specific to Win32 is here:
 * a per-thread TEB, a stack out of the Win32 arena, and the handle table
 * entry a program waits on.
 *
 * Thread *stacks* deliberately come from w32_mem_alloc_pages() rather
 * than being mapped by the scheduler, so they are freed wholesale by
 * w32_mem_reset() when the program exits - no per-task teardown, and
 * nothing for the scheduler's reaper to double-free. */

#define W32_MAX_THREADS 8
#define W32_DEFAULT_THREAD_STACK (64u * 1024u)

typedef struct {
    int      pid;        /* scheduler task, 0 for a free slot */
    uint32_t handle;
    uint32_t exit_code;
    uint32_t teb;
} w32_thread_t;

static w32_thread_t threads[W32_MAX_THREADS];
static uint32_t thread_count = 0;
/* Handles are small integers with a tag, like the other pseudo-handles in
 * win32_internal.h, so a program that prints one gets something sane. */
#define W32_THREAD_HANDLE_BASE 0x10000100u

static void threads_reset(void) {
    for (uint32_t i = 0; i < W32_MAX_THREADS; i++) threads[i].pid = 0;
    thread_count = 0;
}

static w32_thread_t* thread_by_handle(uint32_t handle) {
    for (uint32_t i = 0; i < W32_MAX_THREADS; i++) {
        if (threads[i].pid && threads[i].handle == handle) return &threads[i];
    }
    return 0;
}

int w32_thread_handle_alive(uint32_t handle, uint32_t* exit_code) {
    w32_thread_t* t = thread_by_handle(handle);
    if (!t) return 0;
    if (scheduler_task_alive(t->pid)) return 1;
    if (exit_code) *exit_code = t->exit_code;
    return 0;
}

static w32_thread_t* thread_by_pid(int pid) {
    for (uint32_t i = 0; i < W32_MAX_THREADS; i++) {
        if (threads[i].pid == pid) return &threads[i];
    }
    return 0;
}

uint32_t w32_thread_id(void) {
    return (uint32_t)scheduler_current_pid();
}

/* Records a scheduler task as a thread of this program and hands back its
 * handle. Used for the main thread as well as CreateThread's, so there is
 * one place that knows what a Win32 thread is. */
static uint32_t thread_register(int pid, uint32_t teb) {
    if (thread_count >= W32_MAX_THREADS) return 0;
    w32_thread_t* t = &threads[thread_count];
    t->pid = pid;
    t->handle = W32_THREAD_HANDLE_BASE + thread_count;
    t->exit_code = 0;
    t->teb = teb;
    thread_count++;
    return t->handle;
}

uint32_t w32_thread_create(uint32_t start, uint32_t param, uint32_t stack_size,
                           uint32_t* out_tid) {
    if (!start) return 0;
    if (thread_count >= W32_MAX_THREADS) {
        w32_report("CreateThread: this program already has the maximum "
                   "number of threads");
        return 0;
    }
    if (stack_size == 0) stack_size = W32_DEFAULT_THREAD_STACK;
    if (stack_size < PAGE_SIZE) stack_size = PAGE_SIZE;

    uint32_t stack_low = w32_mem_alloc_pages(stack_size);
    if (!stack_low) return 0;
    uint32_t stack_top = stack_low + stack_size;

    uint32_t teb = build_teb(current_peb, stack_top, stack_low);
    if (!teb) return 0;

    uint32_t tramp = build_thread_exit_trampoline();
    if (!tramp) return 0;

    /* The start routine is stdcall with one argument, and it is *called*,
     * not jumped to: the thread-exit trampoline sits where its return
     * address goes, so `return 0;` from a thread procedure ends the
     * thread exactly the way returning from main ends the program. */
    uint32_t esp = (stack_top - 16) & ~15u;
    esp -= 4; *(uint32_t*)esp = param;
    esp -= 4; *(uint32_t*)esp = tramp;

    int pid = scheduler_spawn_win32_thread("thread", start, esp, teb);
    if (pid < 0) return 0;

    if (out_tid) *out_tid = (uint32_t)pid;
    return thread_register(pid, teb);
}

void w32_thread_exit(uint32_t code) {
    w32_thread_t* t = thread_by_pid(scheduler_current_pid());
    if (t) t->exit_code = code;

    /* Ends this task only. If it was the last one still running, the
     * scheduler unwinds to win32_run_pe() by itself, which is exactly
     * the right behaviour: a program is over when its last thread is.
     *
     * This *must* return normally. scheduler_exit_current() does not
     * switch stacks itself - it records the decision and lets this whole
     * call chain unwind back to isr.s's epilogue, which is the only place
     * it is safe to repoint esp. Halting here instead (as an earlier
     * version did, on the theory that a function called ExitThread should
     * not return) hangs the machine: the switch never happens, and
     * interrupts are off inside an interrupt gate, so the `hlt` is
     * forever. The thread's own thunk never gets to run its `ret`,
     * because by then the epilogue has moved to another task. */
    scheduler_exit_current();
}

int w32_yield_if_others(win32_call_t* call) {
    /* Rewind-and-yield, but only if the yield will actually happen.
     * Sleep() needs this: when it is the only runnable thread there is
     * nothing to gain by spinning, and it should fall back to letting the
     * timer advance while halted. Leaves eip untouched when it returns 0,
     * so the caller's call completes normally. */
    if (!scheduler_yield_would_switch()) return 0;
    w32_retry_call_later(call);
    return 1;
}

void w32_retry_call_later(win32_call_t* call) {
    /* The thunk is `B8 id32` (5 bytes) then `CD 81` (2), and eip in the
     * trap frame points just past the int. Rewinding all 7 re-runs the
     * `mov` too, which is what puts the slot id back in eax - rewinding
     * only the int would re-enter the dispatcher with eax holding this
     * call's *return value* instead. */
    call->regs->eip -= 7;
    scheduler_yield_from_trap(call->regs);
}

pe_load_result_t win32_run_pe(const uint8_t* image, uint32_t size,
                              const char* name, const char* cmdline,
                              int strict, uint32_t* out_exit_code) {
    pe_loader_ops_t ops;
    ops.resolve_import = loader_resolve;
    ops.missing_import = loader_missing;
    ops.ctx = 0;
    ops.strict = strict;

    missing_count = 0; /* the report is per-program, not cumulative */

    /* From here until the matching process_leave_address_space() below,
     * CR3 holds this program's own page directory. Everything the loader
     * and the emulation layer do with ring-3 pointers - mapping the
     * image, zeroing the stack, reading arguments off it inside an
     * int 0x81 - lands in that address space, because the kernel never
     * switches away while it is holding one. See process.h. */
    int owns_as = process_enter_address_space();

    pe_load_result_t r = pe_load(image, size, &ops, &current_image);
    if (r != PE_OK) {
        if (owns_as) process_leave_address_space();
        return r;
    }

    kstrlcpy(current_name, name ? name : "program.exe", sizeof(current_name));
    kstrlcpy(current_cmdline, cmdline ? cmdline : current_name,
             sizeof(current_cmdline));

    if (!map_stack()) {
        pe_unload(&current_image);
        if (owns_as) process_leave_address_space();
        return PE_ERR_OUT_OF_MEMORY;
    }

    uint32_t peb = build_peb(current_image.base);
    current_peb = peb;
    uint32_t stack_low = WIN32_STACK_TOP - WIN32_STACK_SIZE;
    teb_address = build_teb(peb, WIN32_STACK_TOP, stack_low);
    if (!peb || !teb_address) {
        unmap_stack();
        pe_unload(&current_image);
        if (owns_as) process_leave_address_space();
        return PE_ERR_OUT_OF_MEMORY;
    }

    uint32_t exit_tramp = build_exit_trampoline();
    if (!exit_tramp) {
        unmap_stack();
        pe_unload(&current_image);
        if (owns_as) process_leave_address_space();
        return PE_ERR_OUT_OF_MEMORY;
    }

    uint32_t start = current_image.entry;
    if (current_image.tls_callbacks) {
        uint32_t t = build_tls_trampoline(current_image.tls_callbacks,
                                          current_image.base, start);
        if (t) start = t;
    }

    /* The entry point is called, not jumped to: leaving the exit
     * trampoline's address on the stack as its return address is what
     * makes `return 0;` from main() terminate the program cleanly. */
    uint32_t esp = WIN32_STACK_TOP - 16;
    *(uint32_t*)esp = exit_tramp;

    last_error = 0;
    exit_code = 0;
    exception_filter = 0;
    console_attribute = 0x07;
    process_active = 1;
    last_run_faulted = 0;
    w32_kernel32_reset();
    w32_msvcrt_reset();

    /* fs must address the TEB while the program runs (see build_teb) -
     * GDT entry 6, selector 0x33. process_asm.s loads whatever
     * process_set_user_fs() last set. */
    /* Both of these are now the scheduler's job: it repoints the TEB
     * descriptor on every switch (each thread has its own), and the
     * synthetic trap frame it builds carries fs = 0x33 for any task that
     * has a TEB. Set here anyway so the descriptor is valid before the
     * very first task is entered. */
    gdt_set_teb(teb_address, PAGE_SIZE - 1);

    if (owns_as) {
        w32_report_hex("running in its own address space, page directory",
                       process_current_address_space());
    } else {
        w32_report("no private address space available - "
                   "sharing the kernel's, as before Milestone 15");
    }

    uint32_t image_base = current_image.base;

    /* Milestone 17: the program's main thread is a scheduler task, not a
     * one-off process_run_user_mode() call. That is what makes
     * CreateThread possible at all - a second thread is simply a second
     * task in this same address space - and scheduler_run_until_idle()
     * blocks exactly the way process_run_user_mode() used to, so
     * everything around this call is unchanged.
     *
     * The address space is already current, so the task inherits it. */
    threads_reset();
    int main_pid = scheduler_spawn_win32_thread("main", start, esp,
                                                teb_address);
    if (main_pid < 0) {
        unmap_stack();
        pe_unload(&current_image);
        if (owns_as) process_leave_address_space();
        return PE_ERR_OUT_OF_MEMORY;
    }
    thread_register(main_pid, teb_address);

    scheduler_run_until_idle();

    /* Reached only via w32_exit_process() or a fault - both unwind here
     * through process_exit_to_kernel(). */
    process_set_user_fs(0x23);
    process_active = 0;
    /* Both ways out of ring 3 - exiting and faulting - unwind past any
     * win32_callback_call() frames without them returning, so the
     * callback layer's depth counter and its borrowed TSS.esp0 have to be
     * put back by hand here. */
    win32_callback_reset();

    if (missing_count > 0) {
        uint32_t called = 0;
        for (uint32_t i = 0; i < missing_count; i++) {
            if (missing[i].hits) called++;
        }
        terminal_writestring_color("[win32] ", VGA_COLOR_LIGHT_BROWN);
        terminal_writestring("this program imported ");
        char buf[12];
        ku32_to_dec(missing_count, buf);
        terminal_writestring(buf);
        terminal_writestring(" API(s) Novaris doesn't implement; ");
        ku32_to_dec(called, buf);
        terminal_writestring(buf);
        terminal_writestring(" were actually called.\n");
    }

    /* Still standing in the program's address space, deliberately: these
     * unmap ring-3 pages and hand their frames back to the PMM, and they
     * have to be looking at the program's page tables to do it. */
    if (thread_count > 1) {
        w32_report_dec("threads this program ran", thread_count);
        w32_report_dec("times a critical section was found already held",
                       w32_cs_contention_count());
    }

    w32_handles_reset();
    w32_mem_reset();
    unmap_stack();
    pe_unload(&current_image);
    teb_address = 0;
    current_peb = 0;
    threads_reset();
    /* Hand back the TEB/PEB the program was given - see data_reset_mark. */
    if (data_reset_mark) data_next = data_reset_mark;

    /* Only now is it safe to leave. Destroying the directory reclaims
     * anything the four calls above missed, rather than being the only
     * thing that reclaims anything - the teardown paths are unchanged
     * from Milestone 14 and still carry their own weight. */
    if (owns_as) process_leave_address_space();

    /* Back in the kernel's address space now. The program's image must
     * not be reachable from it - if it is, the loader put the image in a
     * page table belonging to the shared kernel set, which would mean the
     * isolation is nominal and every process can see every other one's
     * code. Silent when it holds, which is the normal case; loud when it
     * doesn't, because nothing else would notice. */
    if (owns_as && (paging_get_entry(image_base) & PAGE_PRESENT)) {
        w32_report_hex("LEAK: the image is still mapped in the kernel "
                       "address space at", image_base);
    }

    if (out_exit_code) *out_exit_code = exit_code;
    return PE_OK;
}

/* --- faults ------------------------------------------------------------ */

static const char* fault_name(uint32_t vector) {
    switch (vector) {
        case 0:  return "EXCEPTION_INT_DIVIDE_BY_ZERO";
        case 1:  return "EXCEPTION_SINGLE_STEP";
        case 3:  return "EXCEPTION_BREAKPOINT";
        case 4:  return "EXCEPTION_INT_OVERFLOW";
        case 5:  return "EXCEPTION_ARRAY_BOUNDS_EXCEEDED";
        case 6:  return "EXCEPTION_ILLEGAL_INSTRUCTION";
        case 7:  return "EXCEPTION_NO_FPU";
        case 11: return "EXCEPTION_SEGMENT_NOT_PRESENT";
        case 12: return "EXCEPTION_STACK_OVERFLOW";
        case 13: return "EXCEPTION_PRIV_INSTRUCTION";
        case 14: return "EXCEPTION_ACCESS_VIOLATION";
        case 16: return "EXCEPTION_FLT_INVALID_OPERATION";
        default: return "EXCEPTION_UNKNOWN";
    }
}

int win32_handle_user_fault(registers_t* regs) {
    if (!process_active) return 0;

    char buf[16];
    terminal_writestring("\n");
    terminal_writestring_color("[win32] ", VGA_COLOR_LIGHT_RED);
    terminal_writestring("Unhandled exception in ");
    terminal_writestring(current_name);
    terminal_writestring(":\n        ");
    terminal_writestring(fault_name(regs->int_no));
    terminal_writestring(" at eip=0x");
    ku32_to_hex(regs->eip, buf, 0, 8);
    terminal_writestring(buf);

    if (regs->int_no == 14) {
        uint32_t cr2;
        __asm__ __volatile__("mov %%cr2, %0" : "=r"(cr2));
        terminal_writestring(" accessing 0x");
        ku32_to_hex(cr2, buf, 0, 8);
        terminal_writestring(buf);
    }
    terminal_writestring("\n        image base 0x");
    ku32_to_hex(current_image.base, buf, 0, 8);
    terminal_writestring(buf);
    terminal_writestring(", entry 0x");
    ku32_to_hex(current_image.entry, buf, 0, 8);
    terminal_writestring(buf);
    terminal_writestring("\n");
    terminal_writestring_color("[win32] ", VGA_COLOR_LIGHT_RED);
    terminal_writestring("Program terminated. Returning to the shell.\n");

    /* SEH is not implemented: a real Windows would walk the fs:[0] chain
     * and give the program a chance to handle this. Killing it is the
     * honest alternative - the important part is that a bad .exe takes
     * itself down and not the kernel. */
    last_run_faulted = 1;
    w32_exit_process(0xC0000005u);
    return 1;
}
