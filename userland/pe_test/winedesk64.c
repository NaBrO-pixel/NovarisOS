/* winedesk64.c - windows on a desktop, and a hand on the mouse (Milestone 109).
 *
 * winegui64.exe showed that one window can be made and drawn. This is
 * what comes after: several windows at once, where they were put, in
 * front of and behind each other, and a pointer and a keyboard that
 * reach them. Every program with more than one window needs it - a
 * menu is a window, a tooltip is a window, a dialog is a window in
 * front of another - and a game launcher's whole interface is a
 * borderless popup.
 *
 * Two kinds of evidence, neither of them Wine's opinion of itself:
 *
 *   - **What is on the screen.** /dev/wm answers WMIO_PEEK with the
 *     framebuffer's pixel at a point - after the compositor, not the
 *     window's own surface. A program asking GetPixel would be asking
 *     its own bitmap, which is right even when nothing reaches the
 *     screen.
 *   - **What the hardware did.** WMIO_INJECT feeds the PS/2 decoder the
 *     bytes a mouse or keyboard would send, so a click here is the same
 *     interrupt-path click a person makes. It comes from a second thread
 *     while this one sleeps in MsgWaitForMultipleObjects, so it also
 *     shows that input wakes a sleeping thread - through the listener
 *     the driver gave the wineserver - rather than waiting for a timer.
 *
 * Both are plain Linux syscalls from a Windows program, which is fine:
 * this kernel has no syscall user dispatch for Wine to trap them with.
 *
 * Exit code 0 when everything held; otherwise the number of the first
 * check that did not.
 */

#include <windows.h>
#include <stdio.h>
#include <string.h>

#define LINUX_OPEN  2
#define LINUX_CLOSE 3
#define LINUX_IOCTL 16

#define WMIO_INJECT 0x570Eu
#define WMIO_PEEK   0x570Fu

#define WM_INJECT_POINTER 1
#define WM_INJECT_KEY     2

struct wm_rect   { int x, y, w, h; };
struct wm_inject { unsigned int type; int a, b, c; };

/* Windows is LLP64: `long` is 32 bits, and a pointer passed through one
 * would lose its top half. */
static LONGLONG linux3(LONGLONG n, LONGLONG a, LONGLONG b, LONGLONG c)
{
    LONGLONG ret;
    __asm__ __volatile__("syscall"
                         : "=a"(ret)
                         : "a"(n), "D"(a), "S"(b), "d"(c)
                         : "rcx", "r11", "memory");
    return ret;
}

static int wm_fd = -1;

static int peek(int x, int y)
{
    struct wm_rect r = { x, y, 0, 0 };
    if (linux3(LINUX_IOCTL, wm_fd, WMIO_PEEK, (LONGLONG)(INT_PTR)&r) != 0) return -1;
    return r.w;
}

static void inject_pointer(int x, int y, int buttons)
{
    struct wm_inject in = { WM_INJECT_POINTER, x, y, buttons };
    linux3(LINUX_IOCTL, wm_fd, WMIO_INJECT, (LONGLONG)(INT_PTR)&in);
}

static void inject_key(int keycode, int pressed)
{
    struct wm_inject in = { WM_INJECT_KEY, keycode, pressed, 0 };
    linux3(LINUX_IOCTL, wm_fd, WMIO_INJECT, (LONGLONG)(INT_PTR)&in);
}

static void say(const char *s)
{
    fputs(s, stdout);
    fputc('\n', stdout);
    fflush(stdout);
}

static int failed;
static int check_no;

static void check(const char *what, int ok)
{
    check_no++;
    printf("%s  %s\n", ok ? "ok " : "bad", what);
    fflush(stdout);
    if (!ok && !failed) failed = check_no;
}

/* --- the windows ------------------------------------------------------- */

#define RED   0x00E02020
#define BLUE  0x002020E0
#define GREEN 0x0020C020
#define DESK  0x00305878

static HWND win_a, win_b, popup;
static volatile LONG clicks_a, keys_a, chars_a;
static volatile POINT click_at;
static volatile WCHAR typed[8];

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        COLORREF c = hwnd == win_a ? RGB(0xE0, 0x20, 0x20)
                   : hwnd == win_b ? RGB(0x20, 0x20, 0xE0) : RGB(0x20, 0xC0, 0x20);
        HBRUSH brush = CreateSolidBrush(c);
        RECT r;
        GetClientRect(hwnd, &r);
        FillRect(dc, &r, brush);
        DeleteObject(brush);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN:
        if (hwnd == win_a)
        {
            click_at.x = (short)LOWORD(lp);
            click_at.y = (short)HIWORD(lp);
            InterlockedIncrement(&clicks_a);
        }
        return 0;
    case WM_KEYDOWN:
        if (hwnd == win_a && wp == 'A') InterlockedIncrement(&keys_a);
        return 0;
    case WM_CHAR:
        if (hwnd == win_a && chars_a < 7) typed[chars_a++] = (WCHAR)wp;
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* Messages for up to `ms`, returning early once `done` says so. */
static void pump(int ms, volatile LONG *counter, LONG want)
{
    DWORD start = GetTickCount();
    MSG msg;

    for (;;)
    {
        DWORD now = GetTickCount() - start;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        if (counter && *counter >= want) return;
        if ((int)now >= ms) return;
        MsgWaitForMultipleObjects(0, NULL, FALSE, ms - now, QS_ALLINPUT);
    }
}

/* --- the hand on the mouse -------------------------------------------- */

static POINT a_only, overlap, b_only, popup_pt;
static HANDLE go;

static DWORD WINAPI hand(void *arg)
{
    (void)arg;
    WaitForSingleObject(go, INFINITE);
    Sleep(300);                      /* the window thread is asleep by now */
    inject_pointer(a_only.x, a_only.y, 0);
    inject_pointer(a_only.x, a_only.y, 1);
    inject_pointer(a_only.x, a_only.y, 0);
    inject_pointer(20, 700, 0);      /* out of the way of what is peeked */

    WaitForSingleObject(go, INFINITE);
    Sleep(300);
    inject_key(30, 1);               /* a */
    inject_key(30, 0);
    inject_key(42, 1);               /* shift, b */
    inject_key(48, 1);
    inject_key(48, 0);
    inject_key(42, 0);
    return 0;
}

static POINT client_point(HWND hwnd, int fx, int fy)
{
    RECT r;
    POINT p;
    GetClientRect(hwnd, &r);
    p.x = r.left + (r.right - r.left) * fx / 100;
    p.y = r.top + (r.bottom - r.top) * fy / 100;
    ClientToScreen(hwnd, &p);
    return p;
}

int main(void)
{
    WNDCLASSA wc;
    HANDLE thread;
    DWORD t0, waited;
    RECT ra, rb;
    char line[160];

    wm_fd = (int)linux3(LINUX_OPEN, (LONGLONG)(INT_PTR)"/dev/wm", 2 /* O_RDWR */, 0);
    if (wm_fd < 0)
    {
        say("bad  no /dev/wm");
        return 1;
    }

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc   = wndproc;
    wc.hInstance     = GetModuleHandleA(NULL);
    wc.hCursor       = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    wc.lpszClassName = "NovarisDesk";
    RegisterClassA(&wc);

    win_a = CreateWindowExA(0, "NovarisDesk", "A", WS_OVERLAPPEDWINDOW,
                            100, 100, 300, 200, NULL, NULL, wc.hInstance, NULL);
    win_b = CreateWindowExA(0, "NovarisDesk", "B", WS_OVERLAPPEDWINDOW,
                            250, 180, 300, 200, NULL, NULL, wc.hInstance, NULL);
    /* What a menu, a tooltip and a launcher's main window all are: no
     * caption, no frame, and until Milestone 109 never on the screen. */
    popup = CreateWindowExA(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, "NovarisDesk", "",
                            WS_POPUP, 600, 450, 120, 80, NULL, NULL, wc.hInstance, NULL);
    check("three windows exist", win_a && win_b && popup);
    ShowWindow(win_a, SW_SHOWNORMAL);
    ShowWindow(win_b, SW_SHOWNORMAL);
    ShowWindow(popup, SW_SHOWNOACTIVATE);
    UpdateWindow(win_a);
    UpdateWindow(win_b);
    UpdateWindow(popup);
    pump(1500, NULL, 0);

    GetWindowRect(win_a, &ra);
    GetWindowRect(win_b, &rb);
    snprintf(line, sizeof(line), "     A at (%ld,%ld)-(%ld,%ld), B at (%ld,%ld)-(%ld,%ld)",
             ra.left, ra.top, ra.right, ra.bottom, rb.left, rb.top, rb.right, rb.bottom);
    say(line);

    a_only   = client_point(win_a, 15, 40);
    overlap  = client_point(win_b, 15, 30);
    b_only   = client_point(win_b, 80, 80);
    popup_pt = client_point(popup, 50, 50);
    snprintf(line, sizeof(line), "     on screen: A-only #%06x, overlap #%06x, B-only #%06x, "
             "popup #%06x, desktop #%06x", peek(a_only.x, a_only.y),
             peek(overlap.x, overlap.y), peek(b_only.x, b_only.y),
             peek(popup_pt.x, popup_pt.y), peek(5, 760));
    say(line);

    check("A is drawn where Wine put it", peek(a_only.x, a_only.y) == RED);
    check("B, shown later, is in front where they overlap", peek(overlap.x, overlap.y) == BLUE);
    check("and B is drawn where Wine put it", peek(b_only.x, b_only.y) == BLUE);
    check("a borderless popup is on the screen", peek(popup_pt.x, popup_pt.y) == GREEN);
    check("Wine draws the frame: A's caption is not its client colour",
          peek(ra.left + 40, ra.top + 8) != RED && peek(ra.left + 40, ra.top + 8) != DESK);
    check("the desktop shows where no window is", peek(5, 760) == DESK);

    /* A click on the part of A that shows, while this thread sleeps. */
    go = CreateEventA(NULL, FALSE, FALSE, NULL);
    thread = CreateThread(NULL, 0, hand, NULL, 0, NULL);
    SetEvent(go);
    t0 = GetTickCount();
    pump(5000, &clicks_a, 1);
    waited = GetTickCount() - t0;
    snprintf(line, sizeof(line), "     the click arrived after %lu ms at client (%ld,%ld)",
             waited, click_at.x, click_at.y);
    say(line);
    {
        POINT want = a_only;
        ScreenToClient(win_a, &want);
        check("a click reaches the window under the pointer", clicks_a == 1);
        check("at the point that was clicked",
              click_at.x == want.x && click_at.y == want.y);
    }
    check("and wakes the thread waiting for it, not its timeout", waited < 4000);
    pump(500, NULL, 0);
    check("clicked, A comes to the front", peek(overlap.x, overlap.y) == RED);
    check("and is the active window", GetForegroundWindow() == win_a);

    /* Keys go to the window in front. */
    SetEvent(go);
    pump(5000, &chars_a, 2);
    snprintf(line, sizeof(line), "     typed: %d keydown(s) of A, chars \"%lc%lc\"",
             (int)keys_a, chars_a > 0 ? typed[0] : L'?', chars_a > 1 ? typed[1] : L'?');
    say(line);
    check("a key press is WM_KEYDOWN 'A'", keys_a == 1);
    check("which TranslateMessage makes 'a', and shift+b 'B'",
          chars_a == 2 && typed[0] == L'a' && typed[1] == L'B');

    /* Moved, B leaves what was behind it. */
    SetWindowPos(win_b, NULL, 620, 60, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    pump(500, NULL, 0);
    check("moved away, B's old place shows the desktop", peek(b_only.x, b_only.y) == DESK);
    {
        POINT nb = client_point(win_b, 50, 50);
        check("and B is drawn where it went", peek(nb.x, nb.y) == BLUE);
    }
    ShowWindow(popup, SW_HIDE);
    pump(300, NULL, 0);
    check("a hidden popup is gone from the screen", peek(popup_pt.x, popup_pt.y) == DESK);

    WaitForSingleObject(thread, 2000);
    DestroyWindow(popup);
    DestroyWindow(win_b);
    DestroyWindow(win_a);
    pump(300, NULL, 0);
    check("closed, the desktop is all that is left", peek(a_only.x, a_only.y) == DESK);
    linux3(LINUX_CLOSE, wm_fd, 0, 0);

    if (failed) printf("winedesk64: check %d did not hold\n", failed);
    else say("winedesk64: windows stack, move, and take clicks and keys");
    fflush(stdout);
    return failed;
}
