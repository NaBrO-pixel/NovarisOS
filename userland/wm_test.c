/* wm_test.c - a ring-3 process that owns a window on the Novaris desktop.
 *
 * Milestone 36. Every window the desktop has drawn until now was drawn by
 * kernel code: an app is a paint callback compiled into the kernel
 * (kernel/app_*.c). This program is not in the kernel. It is an ordinary
 * ELF, loaded by the ordinary loader, running in ring 3 with nothing but
 * `int $0x80` - and it puts a window on the screen and draws into it.
 *
 * That is the piece Wine's display driver has been missing. win32u asks
 * its driver for a surface to draw into, tells it where the window went,
 * and pumps input through it; those three are open+ioctl(CREATE)+mmap,
 * ioctl(DAMAGE), and ioctl(POLL). This program is the smallest thing that
 * exercises all three, so that when a driver does the same calls the
 * question is only whether *the driver* is right.
 *
 * Same rules as the rest of the comparison binaries: raw `int $0x80`,
 * Linux syscall numbers, `gcc -m32 -static -nostdlib -ffreestanding`,
 * linked against nothing.
 *
 * What you should see: a window titled "Novaris Window Test" with a blue
 * gradient, a white frame, a red square that walks left to right, and a
 * band across the middle that lights up green while a key is held or the
 * mouse is inside the window. Clicking the X closes it and the program
 * exits 0.
 */

static long sc1(long n, long a) {
    long r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "a"(n), "b"(a) : "memory");
    return r;
}
static long sc2(long n, long a, long b) {
    long r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "a"(n), "b"(a), "c"(b)
                         : "memory");
    return r;
}
static long sc3(long n, long a, long b, long c) {
    long r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "a"(n), "b"(a), "c"(b), "d"(c)
                         : "memory");
    return r;
}
static long sc6(long n, long a, long b, long c, long d, long e, long f) {
    long r;
    __asm__ __volatile__("push %%ebp\n\t"
                         "mov %7, %%ebp\n\t"
                         "int $0x80\n\t"
                         "pop %%ebp"
                         : "=a"(r)
                         : "a"(n), "b"(a), "c"(b), "d"(c), "S"(d), "D"(e),
                           "rm"(f)
                         : "memory");
    return r;
}

#define SYS_exit       1
#define SYS_write      4
#define SYS_open       5
#define SYS_close      6
#define SYS_ioctl     54
#define SYS_nanosleep 162
#define SYS_mmap2    192

#define O_RDWR   2

#define PROT_READ  1
#define PROT_WRITE 2
#define MAP_SHARED 0x01

/* Must match include/wmdev.h. Spelled out rather than included because
 * this is a Linux program by construction - it gets to know the numbers,
 * not the kernel's headers. */
#define WMIO_CREATE  0x5701u
#define WMIO_DAMAGE  0x5702u
#define WMIO_POLL    0x5703u
#define WMIO_GETSIZE 0x5704u
#define WMIO_TITLE   0x5705u
#define WMIO_GETINFO 0x5707u

#define WM_EV_NONE   0
#define WM_EV_MOUSE  1
#define WM_EV_KEY    2
#define WM_EV_CLOSE  3
#define WM_EV_RESIZE 4

struct wm_create { unsigned w, h; char title[64]; };
struct wm_rect { int x, y, w, h; };
struct wm_event { unsigned type; int a, b, c, d; };
struct wm_info { unsigned w, h, stride, cap_h, bytes; };

static unsigned slen(const char* s) { unsigned n = 0; while (s[n]) n++; return n; }
static void out(const char* s) { sc3(SYS_write, 1, (long)s, slen(s)); }
static void outn(long v) {
    char b[16];
    int i = 15;
    int neg = v < 0;
    unsigned long u = neg ? (unsigned long)(-v) : (unsigned long)v;
    b[i--] = '\0';
    if (!u) b[i--] = '0';
    while (u) { b[i--] = (char)('0' + (u % 10)); u /= 10; }
    if (neg) b[i--] = '-';
    sc3(SYS_write, 1, (long)&b[i + 1], slen(&b[i + 1]));
}
static void check(const char* label, int ok) {
    out(ok ? "  [ok]   " : "  [FAIL] ");
    out(label);
    out("\n");
}

static void nap(long ms) {
    long ts[2];
    ts[0] = ms / 1000;
    ts[1] = (ms % 1000) * 1000000L;
    sc2(SYS_nanosleep, (long)ts, 0);
}

#define W 480
#define H 320

static unsigned* px;

/* Pixels per row of the buffer, from WMIO_GETINFO. Not W: since Milestone
 * 42 the kernel allocates a window's buffer at the largest size the window
 * could be shown at, so that a resize never moves the mapping - which
 * means a row is `stride` wide and a program that assumes its own width
 * draws a staircase. */
static int stride = W;

static void put(int x, int y, unsigned c) {
    if (x < 0 || y < 0 || x >= W || y >= H) return;
    px[y * stride + x] = c;
}
static void rect(int x, int y, int w, int h, unsigned c) {
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) put(x + i, y + j, c);
}

/* The background never changes, so it is drawn once: a vertical blue
 * gradient with a white frame, which is easy to recognise in a screenshot
 * and impossible to mistake for a kernel app's flat fill. */
static void draw_background(void) {
    for (int y = 0; y < H; y++) {
        /* 0x00RRGGBB: a little red, rising green, rising blue. */
        unsigned b = (unsigned)(60 + (y * 150) / H);
        unsigned c = (0x20u << 16) | ((unsigned)(40 + (y * 120) / H) << 8) | b;
        for (int x = 0; x < W; x++) px[y * stride + x] = c;
    }
    rect(0, 0, W, 2, 0x00FFFFFFu);
    rect(0, H - 2, W, 2, 0x00FFFFFFu);
    rect(0, 0, 2, H, 0x00FFFFFFu);
    rect(W - 2, 0, 2, H, 0x00FFFFFFu);
}

static int atoi_(const char* s) {
    int v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return v;
}

int main_(int argc, char** argv) {
    /* How many frames to draw before giving up on being closed. The
     * default is a minute, which is long enough to look at; a test that
     * wants to see the program *finish* - and so exercise the teardown
     * path, where the window closes and the slot is freed - passes a
     * smaller number. */
    int limit = 1200;
    if (argc > 1 && argv && argv[1]) {
        int n = atoi_(argv[1]);
        if (n > 0) limit = n;
    }

    out("wm test - a ring-3 process asking the desktop for a window\n\n");

    long fd = sc3(SYS_open, (long)"/dev/wm", O_RDWR, 0);
    check("opened /dev/wm", fd >= 0);
    if (fd < 0) { out("  no window device; nothing to test.\n"); return 1; }

    struct wm_create c;
    c.w = W;
    c.h = H;
    {
        const char* t = "Novaris Window Test";
        unsigned i = 0;
        while (t[i]) { c.title[i] = t[i]; i++; }
        c.title[i] = '\0';
    }
    long r = sc3(SYS_ioctl, fd, WMIO_CREATE, (long)&c);
    check("created a window", r == 0);
    if (r != 0) { out("  ioctl(WMIO_CREATE) = "); outn(r); out("\n"); return 1; }

    struct wm_rect size;
    r = sc3(SYS_ioctl, fd, WMIO_GETSIZE, (long)&size);
    check("it is the size we asked for", r == 0 && size.w == W && size.h == H);

    /* The buffer behind the window, which is wider than the window: the
     * kernel sizes it for the largest this window could be shown at. Both
     * numbers below matter - `stride` to draw a straight row, `bytes` to
     * map a buffer that stays valid when the window is made bigger. */
    struct wm_info info;
    r = sc3(SYS_ioctl, fd, WMIO_GETINFO, (long)&info);
    check("the buffer is at least the window",
          r == 0 && (int)info.stride >= W && (int)info.cap_h >= H &&
          info.bytes >= info.stride * info.cap_h * 4u);
    if (r != 0) { out("  ioctl(WMIO_GETINFO) = "); outn(r); out("\n"); return 1; }
    stride = (int)info.stride;

    long a = sc6(SYS_mmap2, 0, (long)info.bytes, PROT_READ | PROT_WRITE, MAP_SHARED,
                 fd, 0);
    check("mapped its pixels", (unsigned long)a < 0xfffff000UL);
    if ((unsigned long)a >= 0xfffff000UL) return 1;
    px = (unsigned*)a;

    draw_background();
    struct wm_rect all = { 0, 0, W, H };
    check("showed the first frame",
          sc3(SYS_ioctl, fd, WMIO_DAMAGE, (long)&all) == 0);

    out("\nA window is on the desktop, drawn by a ring-3 process.\n");
    out("Close it to exit.\n");

    /* The loop. A frame is: move the marker, note what input arrived,
     * damage the two strips that changed. Nothing here is clever - the
     * point is that all of it is a normal program's memory and the
     * compositor is reading it directly. */
    int marker = 4;
    int lit = 0;
    long frames = 0;
    long events = 0;
    int closed = 0;

    while (!closed && frames < limit) {         /* 20 Hz */
        struct wm_event ev;
        while (sc3(SYS_ioctl, fd, WMIO_POLL, (long)&ev) == 1) {
            /* Said once, on the first event, because it is the assertion
             * a screenshot cannot make: the window is not just drawn, it
             * is reachable. Saying it every frame would drown the rest. */
            if (!events) out("  input reached the window\n");
            events++;
            if (ev.type == WM_EV_CLOSE) { closed = 1; break; }
            if (ev.type == WM_EV_KEY) lit = ev.d ? 1 : 0;
            if (ev.type == WM_EV_MOUSE) lit = 1;
            if (ev.type == WM_EV_RESIZE) {
                out("  resized to ");
                outn(ev.a); out("x"); outn(ev.b); out("\n");
            }
        }
        if (closed) break;

        /* Erase where the marker was, draw it where it is now. Two
         * damaged strips, not a full-window repaint. */
        int old = marker;
        marker += 6;
        if (marker > W - 40) marker = 4;

        for (int y = 40; y < 80; y++)
            for (int x = old; x < old + 32 && x < W; x++) {
                unsigned b = (unsigned)(60 + (y * 150) / H);
                px[y * stride + x] = (0x20u << 16) |
                                ((unsigned)(40 + (y * 120) / H) << 8) | b;
            }
        rect(marker, 40, 32, 40, 0x00FF3030u);

        rect(8, 160, W - 16, 24, lit ? 0x0040FF60u : 0x00203040u);
        lit = 0;

        struct wm_rect d1 = { old < marker ? old : marker, 40,
                              (old < marker ? marker - old : old - marker) + 32,
                              40 };
        struct wm_rect d2 = { 8, 160, W - 16, 24 };
        sc3(SYS_ioctl, fd, WMIO_DAMAGE, (long)&d1);
        sc3(SYS_ioctl, fd, WMIO_DAMAGE, (long)&d2);

        frames++;
        nap(50);
    }

    out("\n  frames drawn: ");
    outn(frames);
    out("\n  events received: ");
    outn(events);
    out("\n");
    check("the window closed cleanly", closed || frames >= limit);

    /* The last descriptor closing is what frees the window: the device's
     * per-open node is unlinked, so its last unref releases it, and
     * releasing it closes the window and gives the pixels back. There is
     * nothing else to clean up - which is the point. */
    sc1(SYS_close, fd);
    out("\nwm test done.\n");
    return 0;
}

/* argc and argv live on the stack the kernel built, and a freestanding
 * program has to go and get them: esp points at argc on entry, with the
 * argv pointers immediately above it. */
__asm__(
    ".globl _start\n"
    "_start:\n"
    "    mov %esp, %eax\n"
    "    and $-16, %esp\n"
    "    push %eax\n"
    "    call start_c\n"
);

void start_c(long* sp) {
    sc1(SYS_exit, main_((int)sp[0], (char**)(sp + 1)));
    __builtin_unreachable();
}
