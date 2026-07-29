/* win32_msvcrt.c - the C runtime an .exe links against.
 *
 * mingw-w64 and MSVC both leave the actual C library outside the binary,
 * so a two-line "hello world" arrives importing thirty-odd symbols from
 * msvcrt.dll (or ucrtbase.dll, or a fistful of api-ms-win-crt-*.dll
 * names, which win32.c aliases onto this one table). Getting those to
 * resolve is most of what makes a real compiled program run.
 *
 * Everything here is cdecl - the caller cleans the stack - which is what
 * makes variadic functions possible at all: printf can read as many
 * arguments as its format string asks for, straight off the ring-3 stack.
 *
 * Two things can't be done from the kernel side and are emitted as native
 * ring-3 code instead (see WCODE and the blobs at the bottom): _initterm
 * and _initterm_e, which call arrays of function pointers belonging to
 * the program, and sqrt/fabs, which must return in st(0). */

#include "win32_internal.h"
#include "kstring.h"
#include "console.h"
#include "vfs.h"
#include "rtc.h"
#include "pit.h"

#define ARG(n) (call->args[n])

/* --- stdio ------------------------------------------------------------- */

/* msvcrt's struct _iobuf, which mingw's stdout/stderr macros index into
 * as _iob[1] / _iob[2]. The layout has to match byte for byte because the
 * program computes those addresses itself. */
typedef struct {
    uint32_t _ptr;
    int32_t  _cnt;
    uint32_t _base;
    int32_t  _flag;
    int32_t  _file;   /* our stream identity: 0/1/2, or a Novaris handle */
    int32_t  _charbuf;
    int32_t  _bufsiz;
    uint32_t _tmpfname;
} w32_iobuf_t;

#define IOB_COUNT 20

static uint32_t iob_address = 0;  /* the _iob data export */
static uint32_t errno_cell = 0;
static uint32_t doserrno_cell = 0;
static uint32_t fmode_cell = 0;
static uint32_t commode_cell = 0;
static uint32_t argc_cell = 0;
static uint32_t argv_cell = 0;
static uint32_t environ_cell = 0;
static uint32_t acmdln_cell = 0;
static uint32_t wcmdln_cell = 0;
static uint32_t argv_block = 0;
static uint32_t rand_state = 1;

/* Shared scratch for one formatted line. Interrupts are disabled inside
 * the dispatcher (int 0x81 is an interrupt gate) and there is one thread,
 * so this can't be re-entered. */
static char format_buffer[2048];

static int stream_id(uint32_t file_ptr) {
    if (!file_ptr) return -1;
    return ((w32_iobuf_t*)file_ptr)->_file;
}

/* Writes to whatever stream a FILE* denotes. Returns bytes written. */
static uint32_t stream_write(uint32_t file_ptr, const char* text, uint32_t len) {
    int id = stream_id(file_ptr);
    if (id == 1 || id == 2 || id == 0) {
        w32_write(text, len);
        return len;
    }
    /* A real file: the initrd is read-only, so writes fail rather than
     * pretending to succeed. */
    return 0;
}

static uint32_t crt_printf(win32_call_t* call) {
    const char* fmt = (const char*)ARG(0);
    uint32_t n = w32_format(format_buffer, sizeof(format_buffer), fmt,
                            &call->args[1]);
    w32_write(format_buffer, n < sizeof(format_buffer) ? n
                                                       : sizeof(format_buffer) - 1);
    return n;
}

static uint32_t crt_fprintf(win32_call_t* call) {
    uint32_t file = ARG(0);
    const char* fmt = (const char*)ARG(1);
    uint32_t n = w32_format(format_buffer, sizeof(format_buffer), fmt,
                            &call->args[2]);
    if (n >= sizeof(format_buffer)) n = sizeof(format_buffer) - 1;
    return stream_write(file, format_buffer, n);
}

static uint32_t crt_vprintf(win32_call_t* call) {
    const char* fmt = (const char*)ARG(0);
    const uint32_t* va = (const uint32_t*)ARG(1);
    uint32_t n = w32_format(format_buffer, sizeof(format_buffer), fmt, va);
    w32_write(format_buffer, n < sizeof(format_buffer) ? n
                                                       : sizeof(format_buffer) - 1);
    return n;
}

static uint32_t crt_vfprintf(win32_call_t* call) {
    uint32_t file = ARG(0);
    const char* fmt = (const char*)ARG(1);
    const uint32_t* va = (const uint32_t*)ARG(2);
    uint32_t n = w32_format(format_buffer, sizeof(format_buffer), fmt, va);
    if (n >= sizeof(format_buffer)) n = sizeof(format_buffer) - 1;
    return stream_write(file, format_buffer, n);
}

static uint32_t crt_sprintf(win32_call_t* call) {
    char* buffer = (char*)ARG(0);
    const char* fmt = (const char*)ARG(1);
    if (!buffer) return 0;
    /* No size limit is the whole hazard of sprintf; 0x7FFFFFFF stands in
     * for "unbounded" so the same engine can serve both forms. */
    return w32_format(buffer, 0x7FFFFFFFu, fmt, &call->args[2]);
}

static uint32_t crt_snprintf(win32_call_t* call) {
    char* buffer = (char*)ARG(0);
    uint32_t size = ARG(1);
    const char* fmt = (const char*)ARG(2);
    if (!buffer || size == 0) return 0;
    return w32_format(buffer, size, fmt, &call->args[3]);
}

static uint32_t crt_vsprintf(win32_call_t* call) {
    char* buffer = (char*)ARG(0);
    const char* fmt = (const char*)ARG(1);
    const uint32_t* va = (const uint32_t*)ARG(2);
    if (!buffer) return 0;
    return w32_format(buffer, 0x7FFFFFFFu, fmt, va);
}

static uint32_t crt_vsnprintf(win32_call_t* call) {
    char* buffer = (char*)ARG(0);
    uint32_t size = ARG(1);
    const char* fmt = (const char*)ARG(2);
    const uint32_t* va = (const uint32_t*)ARG(3);
    if (!buffer || size == 0) return 0;
    return w32_format(buffer, size, fmt, va);
}

static uint32_t crt_puts(win32_call_t* call) {
    const char* s = (const char*)ARG(0);
    if (s) w32_write_str(s);
    w32_write("\n", 1);
    return 0;
}

static uint32_t crt_fputs(win32_call_t* call) {
    const char* s = (const char*)ARG(0);
    if (s) stream_write(ARG(1), s, kstrlen(s));
    return 0;
}

static uint32_t crt_putchar(win32_call_t* call) {
    char c = (char)ARG(0);
    w32_write(&c, 1);
    return (uint32_t)(uint8_t)c;
}

static uint32_t crt_fputc(win32_call_t* call) {
    char c = (char)ARG(0);
    stream_write(ARG(1), &c, 1);
    return (uint32_t)(uint8_t)c;
}

static uint32_t crt_fwrite(win32_call_t* call) {
    const char* data = (const char*)ARG(0);
    uint32_t size = ARG(1);
    uint32_t count = ARG(2);
    uint32_t file = ARG(3);
    if (!data || size == 0) return 0;
    uint32_t written = stream_write(file, data, size * count);
    return written / size;
}

static uint32_t crt_getchar(win32_call_t* call) {
    (void)call;
    char line[2];
    /* No character-at-a-time input path exists; read a line and take its
     * first character, which is what an interactive program sees anyway. */
    if (w32_read_line(line, sizeof(line)) == 0) return 0xFFFFFFFFu; /* EOF */
    return (uint32_t)(uint8_t)line[0];
}

static uint32_t crt_gets(win32_call_t* call) {
    char* buffer = (char*)ARG(0);
    if (!buffer) return 0;
    uint32_t n = w32_read_line(buffer, 1024);
    if (n > 0 && buffer[n - 1] == '\n') buffer[n - 1] = '\0';
    return (uint32_t)buffer;
}

/* fopen(name, mode) - read modes open an initrd file; write modes fail,
 * since there is no writable filesystem (ROADMAP.md Milestone 6). */
static uint32_t crt_fopen(win32_call_t* call) {
    const char* name = (const char*)ARG(0);
    const char* mode = (const char*)ARG(1);
    if (!name || !mode) return 0;
    if (mode[0] != 'r') return 0;

    const char* base = kstrrchr(name, '\\');
    base = base ? base + 1 : name;

    vfs_node_t* node = vfs_root ? vfs_finddir(vfs_root, base) : 0;
    if (!node) return 0;

    uint32_t handle = w32_handle_open(W32_H_FILE, node);
    if (handle == W32_INVALID_HANDLE_VALUE) return 0;

    uint32_t file = w32_mem_alloc_zeroed(sizeof(w32_iobuf_t));
    if (!file) return 0;
    ((w32_iobuf_t*)file)->_file = (int32_t)handle;
    return file;
}

static uint32_t crt_fclose(win32_call_t* call) {
    uint32_t file = ARG(0);
    int id = stream_id(file);
    if (id > 2) w32_handle_close((uint32_t)id);
    if (file) w32_mem_free(file);
    return 0;
}

static uint32_t crt_fread(win32_call_t* call) {
    uint8_t* buffer = (uint8_t*)ARG(0);
    uint32_t size = ARG(1);
    uint32_t count = ARG(2);
    int id = stream_id(ARG(3));
    if (!buffer || size == 0 || id <= 2) return 0;

    w32_handle_t* h = w32_handle_get((uint32_t)id);
    if (!h || h->type != W32_H_FILE) return 0;

    vfs_node_t* node = (vfs_node_t*)h->node;
    uint32_t want = size * count;
    uint32_t remaining = node->length > h->position ? node->length - h->position : 0;
    if (want > remaining) want = remaining;
    uint32_t got = want ? vfs_read(node, h->position, want, buffer) : 0;
    h->position += got;
    return got / size;
}

static uint32_t crt_fseek(win32_call_t* call) {
    int id = stream_id(ARG(0));
    int32_t offset = (int32_t)ARG(1);
    uint32_t origin = ARG(2);
    if (id <= 2) return 0xFFFFFFFFu;

    w32_handle_t* h = w32_handle_get((uint32_t)id);
    if (!h) return 0xFFFFFFFFu;

    uint32_t length = ((vfs_node_t*)h->node)->length;
    int32_t base = (origin == 1) ? (int32_t)h->position
                 : (origin == 2) ? (int32_t)length : 0;
    int32_t target = base + offset;
    if (target < 0) target = 0;
    if ((uint32_t)target > length) target = (int32_t)length;
    h->position = (uint32_t)target;
    return 0;
}

static uint32_t crt_ftell(win32_call_t* call) {
    int id = stream_id(ARG(0));
    if (id <= 2) return 0xFFFFFFFFu;
    w32_handle_t* h = w32_handle_get((uint32_t)id);
    return h ? h->position : 0xFFFFFFFFu;
}

static uint32_t crt_feof(win32_call_t* call) {
    int id = stream_id(ARG(0));
    if (id <= 2) return 0;
    w32_handle_t* h = w32_handle_get((uint32_t)id);
    if (!h) return 1;
    return h->position >= ((vfs_node_t*)h->node)->length;
}

static uint32_t crt_fflush(win32_call_t* call) {
    (void)call;
    return 0; /* output is never buffered here - it goes straight to the console */
}

static uint32_t crt_iob_func(win32_call_t* call) {
    (void)call;
    return iob_address;
}

static uint32_t crt_acrt_iob_func(win32_call_t* call) {
    uint32_t index = ARG(0);
    if (index >= IOB_COUNT) index = 0;
    return iob_address + index * (uint32_t)sizeof(w32_iobuf_t);
}

static uint32_t crt_fileno(win32_call_t* call) {
    return (uint32_t)stream_id(ARG(0));
}

/* --- memory ------------------------------------------------------------ */

static uint32_t crt_malloc(win32_call_t* call)  { return w32_mem_alloc(ARG(0)); }
static uint32_t crt_free(win32_call_t* call)    { w32_mem_free(ARG(0)); return 0; }
static uint32_t crt_realloc(win32_call_t* call) { return w32_mem_realloc(ARG(0), ARG(1)); }
static uint32_t crt_msize(win32_call_t* call)   { return w32_mem_size(ARG(0)); }

static uint32_t crt_calloc(win32_call_t* call) {
    return w32_mem_alloc_zeroed(ARG(0) * ARG(1));
}

static uint32_t crt_memcpy(win32_call_t* call) {
    kmemcpy((void*)ARG(0), (const void*)ARG(1), ARG(2));
    return ARG(0);
}

static uint32_t crt_memmove(win32_call_t* call) {
    kmemmove((void*)ARG(0), (const void*)ARG(1), ARG(2));
    return ARG(0);
}

static uint32_t crt_memset(win32_call_t* call) {
    kmemset((void*)ARG(0), (uint8_t)ARG(1), ARG(2));
    return ARG(0);
}

static uint32_t crt_memcmp(win32_call_t* call) {
    return (uint32_t)kmemcmp((const void*)ARG(0), (const void*)ARG(1), ARG(2));
}

static uint32_t crt_memchr(win32_call_t* call) {
    return (uint32_t)kmemchr((const void*)ARG(0), (uint8_t)ARG(1), ARG(2));
}

/* --- strings ----------------------------------------------------------- */

static uint32_t crt_strlen(win32_call_t* call) {
    const char* s = (const char*)ARG(0);
    return s ? kstrlen(s) : 0;
}

static uint32_t crt_strcpy(win32_call_t* call) {
    kstrcpy((char*)ARG(0), (const char*)ARG(1));
    return ARG(0);
}

static uint32_t crt_strncpy(win32_call_t* call) {
    char* dst = (char*)ARG(0);
    const char* src = (const char*)ARG(1);
    uint32_t n = ARG(2);
    uint32_t i = 0;
    /* Deliberately strncpy's real semantics: pad with NULs, and do NOT
     * terminate if the source is longer than n. */
    for (; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = '\0';
    return ARG(0);
}

static uint32_t crt_strcat(win32_call_t* call) {
    kstrcat((char*)ARG(0), (const char*)ARG(1));
    return ARG(0);
}

static uint32_t crt_strncat(win32_call_t* call) {
    char* dst = (char*)ARG(0);
    const char* src = (const char*)ARG(1);
    uint32_t n = ARG(2);
    uint32_t at = kstrlen(dst);
    uint32_t i = 0;
    for (; i < n && src[i]; i++) dst[at + i] = src[i];
    dst[at + i] = '\0';
    return ARG(0);
}

static uint32_t crt_strcmp(win32_call_t* call) {
    return (uint32_t)kstrcmp((const char*)ARG(0), (const char*)ARG(1));
}

static uint32_t crt_strncmp(win32_call_t* call) {
    return (uint32_t)kstrncmp((const char*)ARG(0), (const char*)ARG(1), ARG(2));
}

static uint32_t crt_stricmp(win32_call_t* call) {
    return (uint32_t)kstricmp((const char*)ARG(0), (const char*)ARG(1));
}

static uint32_t crt_strchr(win32_call_t* call) {
    return (uint32_t)kstrchr((const char*)ARG(0), (char)ARG(1));
}

static uint32_t crt_strrchr(win32_call_t* call) {
    return (uint32_t)kstrrchr((const char*)ARG(0), (char)ARG(1));
}

static uint32_t crt_strstr(win32_call_t* call) {
    return (uint32_t)kstrstr((const char*)ARG(0), (const char*)ARG(1));
}

static uint32_t crt_strdup(win32_call_t* call) {
    return w32_strdup_user((const char*)ARG(0));
}

/* strtok keeps state between calls, exactly as the real one does. */
static char* strtok_state = 0;

static uint32_t crt_strtok(win32_call_t* call) {
    char* s = (char*)ARG(0);
    const char* delim = (const char*)ARG(1);
    if (!s) s = strtok_state;
    if (!s || !delim) return 0;

    while (*s && kstrchr(delim, *s)) s++;
    if (!*s) { strtok_state = 0; return 0; }

    char* start = s;
    while (*s && !kstrchr(delim, *s)) s++;
    if (*s) { *s = '\0'; strtok_state = s + 1; }
    else strtok_state = 0;

    return (uint32_t)start;
}

static uint32_t crt_atoi(win32_call_t* call) {
    return (uint32_t)kstr_to_int((const char*)ARG(0));
}

/* strtol(s, end, base) */
static uint32_t crt_strtol(win32_call_t* call) {
    const char* s = (const char*)ARG(0);
    char** end = (char**)ARG(1);
    uint32_t base = ARG(2);
    if (!s) return 0;

    while (*s == ' ' || (*s >= 9 && *s <= 13)) s++;
    int negative = 0;
    if (*s == '-') { negative = 1; s++; }
    else if (*s == '+') s++;

    if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        s += 2;
    } else if (base == 0) {
        base = (s[0] == '0') ? 8u : 10u;
    }

    int32_t value = 0;
    for (;; s++) {
        uint32_t digit;
        if (*s >= '0' && *s <= '9') digit = (uint32_t)(*s - '0');
        else if (*s >= 'a' && *s <= 'z') digit = (uint32_t)(*s - 'a' + 10);
        else if (*s >= 'A' && *s <= 'Z') digit = (uint32_t)(*s - 'A' + 10);
        else break;
        if (digit >= base) break;
        value = value * (int32_t)base + (int32_t)digit;
    }
    if (end) *end = (char*)s;
    return (uint32_t)(negative ? -value : value);
}

static uint32_t crt_itoa(win32_call_t* call) {
    int32_t value = (int32_t)ARG(0);
    char* buffer = (char*)ARG(1);
    uint32_t base = ARG(2);
    if (!buffer) return 0;

    if (base == 10 && value < 0) {
        buffer[0] = '-';
        ku32_to_dec((uint32_t)(-value), buffer + 1);
    } else if (base == 16) {
        ku32_to_hex((uint32_t)value, buffer, 0, 0);
    } else {
        ku32_to_dec((uint32_t)value, buffer);
    }
    return (uint32_t)buffer;
}

static uint32_t crt_abs(win32_call_t* call) {
    int32_t v = (int32_t)ARG(0);
    return (uint32_t)(v < 0 ? -v : v);
}

static uint32_t crt_strnlen(win32_call_t* call) {
    return kstrnlen((const char*)ARG(0), ARG(1));
}

static uint32_t crt_strnicmp(win32_call_t* call) {
    const char* a = (const char*)ARG(0);
    const char* b = (const char*)ARG(1);
    uint32_t n = ARG(2);
    if (!a || !b) return 0;
    for (uint32_t i = 0; i < n; i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
        if (x != y) return (uint32_t)((int)(uint8_t)x - (int)(uint8_t)y);
        if (!x) break;
    }
    return 0;
}

static uint32_t crt_strspn(win32_call_t* call) {
    const char* s = (const char*)ARG(0);
    const char* accept = (const char*)ARG(1);
    if (!s || !accept) return 0;
    uint32_t n = 0;
    while (s[n] && kstrchr(accept, s[n])) n++;
    return n;
}

static uint32_t crt_strcspn(win32_call_t* call) {
    const char* s = (const char*)ARG(0);
    const char* reject = (const char*)ARG(1);
    if (!s || !reject) return 0;
    uint32_t n = 0;
    while (s[n] && !kstrchr(reject, s[n])) n++;
    return n;
}

static uint32_t crt_strpbrk(win32_call_t* call) {
    const char* s = (const char*)ARG(0);
    const char* accept = (const char*)ARG(1);
    if (!s || !accept) return 0;
    for (; *s; s++) {
        if (kstrchr(accept, *s)) return (uint32_t)s;
    }
    return 0;
}

static uint32_t crt_strlwr(win32_call_t* call) {
    char* s = (char*)ARG(0);
    if (!s) return 0;
    for (char* p = s; *p; p++) {
        if (*p >= 'A' && *p <= 'Z') *p = (char)(*p + 32);
    }
    return (uint32_t)s;
}

static uint32_t crt_strupr(win32_call_t* call) {
    char* s = (char*)ARG(0);
    if (!s) return 0;
    for (char* p = s; *p; p++) {
        if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 32);
    }
    return (uint32_t)s;
}

static uint32_t crt_wcscmp(win32_call_t* call) {
    const uint16_t* a = (const uint16_t*)ARG(0);
    const uint16_t* b = (const uint16_t*)ARG(1);
    if (!a || !b) return 0;
    while (*a && *a == *b) { a++; b++; }
    return (uint32_t)((int32_t)*a - (int32_t)*b);
}

static uint32_t crt_wcscpy(win32_call_t* call) {
    uint16_t* dst = (uint16_t*)ARG(0);
    const uint16_t* src = (const uint16_t*)ARG(1);
    if (!dst || !src) return ARG(0);
    uint32_t i = 0;
    while ((dst[i] = src[i])) i++;
    return ARG(0);
}

static uint32_t crt_wcschr(win32_call_t* call) {
    const uint16_t* s = (const uint16_t*)ARG(0);
    uint16_t c = (uint16_t)ARG(1);
    if (!s) return 0;
    for (;; s++) {
        if (*s == c) return (uint32_t)s;
        if (!*s) return 0;
    }
}

static uint32_t crt_toupper(win32_call_t* call) {
    uint32_t c = ARG(0);
    return (c >= 'a' && c <= 'z') ? c - 32 : c;
}

static uint32_t crt_tolower(win32_call_t* call) {
    uint32_t c = ARG(0);
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

/* --- process, time, misc ----------------------------------------------- */

static uint32_t crt_exit(win32_call_t* call) {
    w32_exit_process(ARG(0));
    return 0;
}

static uint32_t crt_abort(win32_call_t* call) {
    (void)call;
    w32_write_str("\nabort() called\n");
    w32_exit_process(3);
    return 0;
}

static uint32_t crt_time(win32_call_t* call) {
    uint32_t now = rtc_unix_time();
    uint32_t* out = (uint32_t*)ARG(0);
    if (out) *out = now;
    return now;
}

static uint32_t crt_clock(win32_call_t* call) {
    (void)call;
    return pit_get_ticks() * 10; /* CLOCKS_PER_SEC is 1000 on Windows */
}

static uint32_t crt_rand(win32_call_t* call) {
    (void)call;
    /* The exact LCG msvcrt uses, so a program seeded the same way gets
     * the same sequence it would on Windows. */
    rand_state = rand_state * 214013u + 2531011u;
    return (rand_state >> 16) & 0x7FFF;
}

static uint32_t crt_srand(win32_call_t* call) {
    rand_state = ARG(0);
    return 0;
}

static uint32_t crt_errno(win32_call_t* call) {
    (void)call;
    return errno_cell;
}

/* The __p_* family returns the address of a CRT variable. mingw's startup
 * code uses these instead of importing the variables directly, so each
 * one just hands back the data export's storage. */
static uint32_t crt_p_fmode(win32_call_t* call)   { (void)call; return fmode_cell; }
static uint32_t crt_p_commode(win32_call_t* call) { (void)call; return commode_cell; }
static uint32_t crt_p_argc(win32_call_t* call)    { (void)call; return argc_cell; }
static uint32_t crt_p_argv(win32_call_t* call)    { (void)call; return argv_cell; }
static uint32_t crt_p_environ(win32_call_t* call) { (void)call; return environ_cell; }

/* __p__acmdln returns a char** - the address of the CRT's own pointer to
 * the command line, which the caller then dereferences. So the cell has
 * to be populated, not merely non-null. mingw's WinMain startup path uses
 * this instead of GetCommandLineA. */
static uint32_t crt_p_acmdln(win32_call_t* call) {
    (void)call;
    if (acmdln_cell && !*(uint32_t*)acmdln_cell) {
        *(uint32_t*)acmdln_cell = w32_strdup_user(w32_current_cmdline());
    }
    return acmdln_cell;
}

static uint32_t crt_p_wcmdln(win32_call_t* call) {
    (void)call;
    if (wcmdln_cell && !*(uint32_t*)wcmdln_cell) {
        const char* text = w32_current_cmdline();
        uint32_t chars = kstrlen(text) + 1;
        uint32_t wide = w32_mem_alloc(chars * 2);
        if (wide) w32_ascii_to_wide(text, (uint16_t*)wide, chars);
        *(uint32_t*)wcmdln_cell = wide;
    }
    return wcmdln_cell;
}

static uint32_t lconv_block = 0;
static uint32_t locale_name = 0;

/* localeconv() matters far more than its obscurity suggests.
 *
 * mingw-w64 does not forward printf to msvcrt for floating-point
 * conversions - it links its own __pformat engine into the .exe, and that
 * engine calls localeconv() and dereferences the decimal_point field to
 * find out which character to use for the radix point. A stub returning
 * NULL turns the first "%f" into a null-pointer crash inside the
 * program's own code, which is exactly what happened before this existed.
 *
 * struct lconv on Windows is ten char* fields followed by a run of char
 * flags; CHAR_MAX (0x7F) in a flag means "not available in this locale",
 * which is the correct answer for all of them here. */
static uint32_t crt_localeconv(win32_call_t* call) {
    (void)call;
    if (lconv_block) return lconv_block;

    lconv_block = w32_mem_alloc_zeroed(128);
    if (!lconv_block) return 0;

    /* The strings live in the tail of the same allocation. */
    uint32_t dot = lconv_block + 64;
    uint32_t empty = lconv_block + 66;
    kstrcpy((char*)dot, ".");
    kstrcpy((char*)empty, "");

    uint32_t* fields = (uint32_t*)lconv_block;
    fields[0] = dot;   /* decimal_point */
    fields[1] = empty; /* thousands_sep */
    fields[2] = empty; /* grouping */
    fields[3] = empty; /* int_curr_symbol */
    fields[4] = empty; /* currency_symbol */
    fields[5] = dot;   /* mon_decimal_point */
    fields[6] = empty; /* mon_thousands_sep */
    fields[7] = empty; /* mon_grouping */
    fields[8] = empty; /* positive_sign */
    fields[9] = empty; /* negative_sign */

    for (uint32_t i = 40; i < 56; i++) ((uint8_t*)lconv_block)[i] = 0x7F;

    return lconv_block;
}

static uint32_t crt_setlocale(win32_call_t* call) {
    (void)call;
    /* Only the "C" locale exists, and it's already in effect. Returning
     * its name rather than NULL is what tells a caller that succeeded. */
    if (!locale_name) locale_name = w32_strdup_user("C");
    return locale_name;
}

/* mbrtowc(wchar_t* out, const char* s, size_t n, mbstate_t* state) - the
 * other half of what mingw's printf needs: single-byte code page, so one
 * byte in is one wide character out. */
static uint32_t crt_mbrtowc(win32_call_t* call) {
    uint16_t* out = (uint16_t*)ARG(0);
    const char* s = (const char*)ARG(1);
    uint32_t n = ARG(2);

    if (!s) return 0;      /* "s is null": the state is always initial here */
    if (n == 0) return (uint32_t)-2; /* incomplete */
    if (out) *out = (uint16_t)(uint8_t)s[0];
    return s[0] ? 1u : 0u;
}

/* wcrtomb(char* out, wchar_t c, mbstate_t* state) */
static uint32_t crt_wcrtomb(win32_call_t* call) {
    char* out = (char*)ARG(0);
    uint32_t c = ARG(1);
    if (!out) return 1;
    *out = c < 256 ? (char)c : '?';
    return 1;
}

static uint32_t crt_btowc(win32_call_t* call) {
    return ARG(0) & 0xFF;
}

static uint32_t strerror_text = 0;

static uint32_t crt_strerror(win32_call_t* call) {
    (void)call;
    /* One message for every errno: nothing here sets a meaningful one.
     * It has to be copied into user-visible memory - the kernel's own
     * string constants live at 0xC0000000+, which ring 3 can't read. */
    if (!strerror_text) {
        strerror_text = w32_strdup_user("Novaris: no error detail available");
    }
    return strerror_text;
}

static uint32_t crt_wcslen(win32_call_t* call) {
    return w32_wide_len((const uint16_t*)ARG(0));
}

static uint32_t crt_doserrno(win32_call_t* call) {
    (void)call;
    return doserrno_cell;
}

static uint32_t crt_getenv(win32_call_t* call) {
    (void)call;
    return 0; /* the environment block is empty of anything worth returning */
}

static uint32_t crt_system(win32_call_t* call) {
    (void)call;
    return 0xFFFFFFFFu; /* no command processor */
}

/* __getmainargs(argc*, argv*, env*, doWildCard, startupinfo*) - how a
 * mingw program actually obtains its arguments. */
static uint32_t crt_getmainargs(win32_call_t* call) {
    int32_t* argc = (int32_t*)ARG(0);
    uint32_t* argv = (uint32_t*)ARG(1);
    uint32_t* env = (uint32_t*)ARG(2);

    if (!argv_block) {
        /* One allocation holding the pointer array followed by the
         * command-line text it points into. */
        const char* cmdline = w32_current_cmdline();
        uint32_t text_len = kstrlen(cmdline) + 1;
        argv_block = w32_mem_alloc_zeroed(4 * 4 + text_len);
        if (!argv_block) return 0xFFFFFFFFu;

        uint32_t text = argv_block + 16;
        kmemcpy((void*)text, cmdline, text_len);

        /* Only the program name is split out - there's no full command
         * line tokenizer here, and the shell passes the name alone. */
        uint32_t* vector = (uint32_t*)argv_block;
        vector[0] = text;
        vector[1] = 0;
        vector[2] = 0; /* the (empty) environment vector */
    }

    if (argc) *argc = 1;
    if (argv) *argv = argv_block;
    if (env) *env = argv_block + 8;
    return 0;
}

/* --- native ring-3 code blobs ------------------------------------------ */

/* _initterm(void (**start)(void), void (**end)(void)) - walks an array of
 * function pointers and calls each non-null one. Those functions belong
 * to the program, so this has to be ring-3 code: the kernel is in the
 * middle of an interrupt when the call arrives and cannot call back out
 * to user mode. See win32.h's WCODE.
 *
 *   00  53             push ebx
 *   01  56             push esi
 *   02  8B 74 24 0C    mov esi, [esp+12]   ; start (past ret + 2 pushes)
 *   06  8B 5C 24 10    mov ebx, [esp+16]   ; end
 *   0A  39 DE          cmp esi, ebx        ; loop:
 *   0C  73 09          jae done
 *   0E  AD             lodsd               ; eax = *esi++
 *   0F  85 C0          test eax, eax
 *   11  74 F7          jz loop             ; null entries are skipped
 *   13  FF D0          call eax
 *   15  EB F3          jmp loop
 *   17  5E             pop esi             ; done:
 *   18  5B             pop ebx
 *   19  C3             ret
 */
static const uint8_t code_initterm[] = {
    0x53, 0x56,
    0x8B, 0x74, 0x24, 0x0C,
    0x8B, 0x5C, 0x24, 0x10,
    0x39, 0xDE,
    0x73, 0x09,
    0xAD,
    0x85, 0xC0,
    0x74, 0xF7,
    0xFF, 0xD0,
    0xEB, 0xF3,
    0x5E, 0x5B, 0xC3,
};

/* _initterm_e is the same walk over functions returning int, stopping at
 * the first non-zero result and returning it.
 *
 *   00  53             push ebx
 *   01  56             push esi
 *   02  8B 74 24 0C    mov esi, [esp+12]
 *   06  8B 5C 24 10    mov ebx, [esp+16]
 *   0A  39 DE          cmp esi, ebx        ; loop:
 *   0C  73 0D          jae ok
 *   0E  AD             lodsd
 *   0F  85 C0          test eax, eax
 *   11  74 F7          jz loop
 *   13  FF D0          call eax
 *   15  85 C0          test eax, eax
 *   17  75 04          jnz done            ; propagate the failure
 *   19  EB EF          jmp loop
 *   1B  31 C0          xor eax, eax        ; ok:
 *   1D  5E             pop esi             ; done:
 *   1E  5B             pop ebx
 *   1F  C3             ret
 */
static const uint8_t code_initterm_e[] = {
    0x53, 0x56,
    0x8B, 0x74, 0x24, 0x0C,
    0x8B, 0x5C, 0x24, 0x10,
    0x39, 0xDE,
    0x73, 0x0D,
    0xAD,
    0x85, 0xC0,
    0x74, 0xF7,
    0xFF, 0xD0,
    0x85, 0xC0,
    0x75, 0x04,
    0xEB, 0xEF,
    0x31, 0xC0,
    0x5E, 0x5B, 0xC3,
};

/* Functions returning a double return it in st(0), which a C handler in
 * the kernel has no way to set - and the kernel deliberately never
 * touches the FPU (see win32_dtoa.c). Emitting the x87 instruction as
 * ring-3 code sidesteps both problems.
 *
 *   sqrt:  DD 44 24 04   fld qword [esp+4]
 *          D9 FA         fsqrt
 *          C3            ret
 */
static const uint8_t code_sqrt[] = { 0xDD, 0x44, 0x24, 0x04, 0xD9, 0xFA, 0xC3 };
/*   fabs:  DD 44 24 04 / D9 E1 (fabs) / C3 */
static const uint8_t code_fabs[] = { 0xDD, 0x44, 0x24, 0x04, 0xD9, 0xE1, 0xC3 };

/* --- init and reset ---------------------------------------------------- */

void w32_msvcrt_init(void) {
    int msvcrt = w32_module_index("msvcrt.dll");
    if (msvcrt < 0) return;

    iob_address = w32_export_address(msvcrt, "_iob", 0);
    errno_cell = w32_export_address(msvcrt, "_errno_storage", 0);
    doserrno_cell = w32_export_address(msvcrt, "_doserrno_storage", 0);
    fmode_cell = w32_export_address(msvcrt, "_fmode", 0);
    commode_cell = w32_export_address(msvcrt, "_commode", 0);
    argc_cell = w32_export_address(msvcrt, "__argc", 0);
    argv_cell = w32_export_address(msvcrt, "__argv", 0);
    environ_cell = w32_export_address(msvcrt, "_environ", 0);
    acmdln_cell = w32_export_address(msvcrt, "_acmdln", 0);
    wcmdln_cell = w32_export_address(msvcrt, "_wcmdln", 0);

    /* Single-byte character set, which is what everything else here
     * assumes; the CRT reads this to decide how to walk strings. */
    uint32_t mb_cur_max = w32_export_address(msvcrt, "__mb_cur_max", 0);
    if (mb_cur_max) *(uint32_t*)mb_cur_max = 1;

    /* stdin/stdout/stderr are _iob[0..2], distinguished by _file. */
    if (iob_address) {
        w32_iobuf_t* iob = (w32_iobuf_t*)iob_address;
        for (int i = 0; i < IOB_COUNT; i++) {
            iob[i]._file = i < 3 ? i : -1;
            iob[i]._flag = i < 3 ? 0x40 : 0; /* _IOWRT-ish; nothing reads it */
        }
    }
}

void w32_msvcrt_reset(void) {
    argv_block = 0;
    /* The command-line cells point into the per-process heap arena, which
     * is freed between programs, so they get repopulated on demand. */
    if (acmdln_cell) *(uint32_t*)acmdln_cell = 0;
    if (wcmdln_cell) *(uint32_t*)wcmdln_cell = 0;
    strerror_text = 0;
    lconv_block = 0;
    locale_name = 0;
    strtok_state = 0;
    rand_state = 1;
    if (errno_cell) *(uint32_t*)errno_cell = 0;
    if (doserrno_cell) *(uint32_t*)doserrno_cell = 0;
}

/* --- export table ------------------------------------------------------ */

static const win32_export_t msvcrt_exports[] = {
    /* stdio */
    WEXP_C("printf", crt_printf),
    WEXP_C("_printf", crt_printf),
    WEXP_C("wprintf", crt_printf),
    WEXP_C("fprintf", crt_fprintf),
    WEXP_C("vprintf", crt_vprintf),
    WEXP_C("vfprintf", crt_vfprintf),
    WEXP_C("sprintf", crt_sprintf),
    WEXP_C("_snprintf", crt_snprintf),
    WEXP_C("snprintf", crt_snprintf),
    WEXP_C("vsprintf", crt_vsprintf),
    WEXP_C("_vsnprintf", crt_vsnprintf),
    WEXP_C("vsnprintf", crt_vsnprintf),
    WEXP_C("puts", crt_puts),
    WEXP_C("fputs", crt_fputs),
    WEXP_C("putchar", crt_putchar),
    WEXP_C("_putchar", crt_putchar),
    WEXP_C("fputc", crt_fputc),
    WEXP_C("putc", crt_fputc),
    WEXP_C("fwrite", crt_fwrite),
    WEXP_C("getchar", crt_getchar),
    WEXP_C("gets", crt_gets),
    WEXP_C("fopen", crt_fopen),
    WEXP_C("fclose", crt_fclose),
    WEXP_C("fread", crt_fread),
    WEXP_C("fseek", crt_fseek),
    WEXP_C("ftell", crt_ftell),
    WEXP_C("feof", crt_feof),
    WEXP_C("fflush", crt_fflush),
    WEXP_C("__iob_func", crt_iob_func),
    WEXP_C("__acrt_iob_func", crt_acrt_iob_func),
    WEXP_C("_fileno", crt_fileno),
    WSTUB_C("ferror", 0),
    WSTUB_C("setvbuf", 0),
    WSTUB_C("setbuf", 0),
    WSTUB_C("scanf", 0),
    WSTUB_C("sscanf", 0),
    WSTUB_C("fscanf", 0),
    WSTUB_C("remove", 0xFFFFFFFFu),
    WSTUB_C("rename", 0xFFFFFFFFu),

    /* memory */
    WEXP_C("malloc", crt_malloc),
    WEXP_C("calloc", crt_calloc),
    WEXP_C("realloc", crt_realloc),
    WEXP_C("free", crt_free),
    WEXP_C("_msize", crt_msize),
    WEXP_C("memcpy", crt_memcpy),
    WEXP_C("memmove", crt_memmove),
    WEXP_C("memset", crt_memset),
    WEXP_C("memcmp", crt_memcmp),
    WEXP_C("memchr", crt_memchr),

    /* strings */
    WEXP_C("strlen", crt_strlen),
    WEXP_C("strcpy", crt_strcpy),
    WEXP_C("strncpy", crt_strncpy),
    WEXP_C("strcat", crt_strcat),
    WEXP_C("strncat", crt_strncat),
    WEXP_C("strcmp", crt_strcmp),
    WEXP_C("strncmp", crt_strncmp),
    WEXP_C("_stricmp", crt_stricmp),
    WEXP_C("stricmp", crt_stricmp),
    WEXP_C("_strcmpi", crt_stricmp),
    WEXP_C("strchr", crt_strchr),
    WEXP_C("strrchr", crt_strrchr),
    WEXP_C("strstr", crt_strstr),
    WEXP_C("_strdup", crt_strdup),
    WEXP_C("strdup", crt_strdup),
    WEXP_C("strtok", crt_strtok),
    WEXP_C("atoi", crt_atoi),
    WEXP_C("atol", crt_atoi),
    WEXP_C("strtol", crt_strtol),
    WEXP_C("strtoul", crt_strtol),
    WEXP_C("_itoa", crt_itoa),
    WEXP_C("itoa", crt_itoa),
    WEXP_C("_ltoa", crt_itoa),
    WEXP_C("abs", crt_abs),
    WEXP_C("labs", crt_abs),
    WEXP_C("strnlen", crt_strnlen),
    WEXP_C("_strnicmp", crt_strnicmp),
    WEXP_C("strnicmp", crt_strnicmp),
    WEXP_C("strspn", crt_strspn),
    WEXP_C("strcspn", crt_strcspn),
    WEXP_C("strpbrk", crt_strpbrk),
    WEXP_C("_strlwr", crt_strlwr),
    WEXP_C("_strupr", crt_strupr),
    WEXP_C("wcscmp", crt_wcscmp),
    WEXP_C("wcscpy", crt_wcscpy),
    WEXP_C("wcschr", crt_wcschr),
    /* Single-byte code page throughout, so no byte is ever a lead byte. */
    WSTUB_C("_ismbblead", 0),
    WSTUB_C("_ismbbtrail", 0),
    WSTUB_C("isleadbyte", 0),
    WEXP_C("toupper", crt_toupper),
    WEXP_C("tolower", crt_tolower),
    WEXP_C("_toupper", crt_toupper),
    WEXP_C("_tolower", crt_tolower),
    WSTUB_C("atof", 0),
    WSTUB_C("strtod", 0),
    WSTUB_C("isalpha", 0),
    WSTUB_C("isdigit", 0),
    WSTUB_C("isspace", 0),
    WSTUB_C("_isctype", 0),
    WSTUB_C("__pctype_func", 0),

    /* process and environment */
    WEXP_C("exit", crt_exit),
    WEXP_C("_exit", crt_exit),
    WEXP_C("_cexit", crt_exit),
    WEXP_C("abort", crt_abort),
    WEXP_C("__getmainargs", crt_getmainargs),
    WEXP_C("__wgetmainargs", crt_getmainargs),
    WEXP_C("getenv", crt_getenv),
    WEXP_C("system", crt_system),
    WEXP_C("_errno", crt_errno),
    WEXP_C("__doserrno", crt_doserrno),
    WEXP_C("__p__fmode", crt_p_fmode),
    WEXP_C("__p__commode", crt_p_commode),
    WEXP_C("__p___argc", crt_p_argc),
    WEXP_C("__p___argv", crt_p_argv),
    WEXP_C("__p__environ", crt_p_environ),
    WEXP_C("__p__wenviron", crt_p_environ),
    WEXP_C("__p__acmdln", crt_p_acmdln),
    WEXP_C("__p__wcmdln", crt_p_wcmdln),
    WEXP_C("strerror", crt_strerror),
    WEXP_C("_strerror", crt_strerror),
    WEXP_C("wcslen", crt_wcslen),
    WEXP_C("time", crt_time),
    WEXP_C("_time32", crt_time),
    WEXP_C("clock", crt_clock),
    WEXP_C("rand", crt_rand),
    WEXP_C("srand", crt_srand),
    WSTUB_C("atexit", 0),
    WSTUB_C("_onexit", 0),
    WSTUB_C("__dllonexit", 0),
    WSTUB_C("signal", 0),
    WSTUB_C("raise", 0),
    WSTUB_C("_set_app_type", 0),
    WSTUB_C("__set_app_type", 0),
    WSTUB_C("__setusermatherr", 0),
    WSTUB_C("_setusermatherr", 0),
    WSTUB_C("_configthreadlocale", 0),
    WSTUB_C("_controlfp", 0x9001F),
    WSTUB_C("_controlfp_s", 0),
    WSTUB_C("__mingw_setusermatherr", 0),
    WSTUB_C("_amsg_exit", 0),
    WSTUB_C("_XcptFilter", 0),
    WSTUB_C("__C_specific_handler", 0),
    WSTUB_C("_lock", 0),
    WSTUB_C("_unlock", 0),
    WEXP_C("setlocale", crt_setlocale),
    WEXP_C("_setlocale", crt_setlocale),
    WEXP_C("localeconv", crt_localeconv),
    WEXP_C("mbrtowc", crt_mbrtowc),
    WEXP_C("wcrtomb", crt_wcrtomb),
    WEXP_C("btowc", crt_btowc),
    WEXP_C("mbtowc", crt_mbrtowc),
    WSTUB_C("_beginthreadex", 0),
    WSTUB_C("_endthreadex", 0),
    WSTUB_C("__security_init_cookie", 0),
    WSTUB_C("_crt_atexit", 0),
    WSTUB_C("_register_onexit_function", 0),
    WSTUB_C("_initialize_onexit_table", 0),
    WSTUB_C("_get_initial_narrow_environment", 0),
    WSTUB_C("_configure_narrow_argv", 0),
    WSTUB_C("_initialize_narrow_environment", 0),
    WSTUB_C("_set_fmode", 0),
    WSTUB_C("_write", 0),
    WSTUB_C("_read", 0),
    WSTUB_C("_get_osfhandle", 0xFFFFFFFFu),
    WSTUB_C("_isatty", 1),
    WSTUB_C("_setmode", 0),
    WSTUB_C("_seh_filter_exe", 0),

    /* Deliberately absent: qsort, bsearch and atexit's callback list.
     * Each takes a function pointer the C library is supposed to *call*,
     * and the kernel cannot call into ring 3 from inside an interrupt
     * (see the note on _initterm above - that one is solved by emitting
     * ring-3 code, which doesn't generalize to an arbitrary signature).
     * Stubbing them would mean qsort silently not sorting, which is far
     * worse than the "unimplemented API called" report a missing import
     * produces. See ROADMAP.md Milestone 10. */

    /* native ring-3 implementations */
    WCODE("_initterm", code_initterm),
    WCODE("_initterm_e", code_initterm_e),
    WCODE("sqrt", code_sqrt),
    WCODE("fabs", code_fabs),

    /* variables. The CRT reads and writes these directly through the
     * import address table, so they need real storage rather than a
     * thunk - see WDATA in win32.h. */
    WDATA("_iob", IOB_COUNT * 32),
    WDATA("_fmode", 4),
    WDATA("_commode", 4),
    WDATA("__initenv", 4),
    WDATA("_environ", 4),
    WDATA("__winitenv", 4),
    WDATA("_acmdln", 4),
    WDATA("_wcmdln", 4),
    WDATA("_pgmptr", 4),
    WDATA("_osver", 4),
    WDATA("_winmajor", 4),
    WDATA("_winminor", 4),
    WDATA("__argc", 4),
    WDATA("__argv", 4),
    /* A variable, not a function - the CRT reads it to decide whether a
     * string can contain multi-byte characters. */
    WDATA("__mb_cur_max", 4),
    WDATA("_daylight", 4),
    WDATA("_timezone", 4),
    WDATA("_tzname", 8),
    /* Backing storage for _errno()/__doserrno(), which return pointers.
     * Not a name any program imports - it just reuses the data-export
     * machinery to get a page-backed cell. */
    WDATA("_errno_storage", 4),
    WDATA("_doserrno_storage", 4),
};

const win32_module_t win32_module_msvcrt = {
    "msvcrt.dll", msvcrt_exports,
    sizeof(msvcrt_exports) / sizeof(msvcrt_exports[0]), 0
};
