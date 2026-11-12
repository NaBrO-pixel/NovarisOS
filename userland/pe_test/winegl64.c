/* winegl64.c - a Windows OpenGL program, in a window (Milestone 110).
 *
 * What every OpenGL game and launcher starts with: a window, a pixel
 * format, a WGL context, a frame drawn and SwapBuffers. Under Wine 11 on
 * a display driver with no OpenGL of its own, win32u does the rendering
 * through Mesa's surfaceless EGL (llvmpipe) and copies each swapped frame
 * into the window's surface - which winenovaris.drv already puts on the
 * screen. So the check is the screen itself: WMIO_PEEK reads the
 * composited framebuffer, the same way winedesk64.exe does.
 *
 * Exit code 0 when everything held; otherwise the number of the first
 * check that did not.
 */

#include <windows.h>
#include <GL/gl.h>
#include <stdio.h>
#include <string.h>

#define LINUX_OPEN  2
#define LINUX_CLOSE 3
#define LINUX_IOCTL 16
#define WMIO_PEEK   0x570Fu

struct wm_rect { int x, y, w, h; };

static LONGLONG linux3(LONGLONG n, LONGLONG a, LONGLONG b, LONGLONG c)
{
    LONGLONG ret;
    __asm__ __volatile__("syscall" : "=a"(ret) : "a"(n), "D"(a), "S"(b), "d"(c)
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

static int failed, check_no;

static void check(const char *what, int ok)
{
    check_no++;
    printf("%s  %s\n", ok ? "ok " : "bad", what);
    fflush(stdout);
    if (!ok && !failed) failed = check_no;
}

static void pump(int ms)
{
    DWORD start = GetTickCount();
    MSG msg;
    while ((int)(GetTickCount() - start) < ms)
    {
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        MsgWaitForMultipleObjects(0, NULL, FALSE, 50, QS_ALLINPUT);
    }
}

static void draw(int w, int h)
{
    glViewport(0, 0, w, h);
    glClearColor(0.25f, 0.5f, 0.75f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glBegin(GL_TRIANGLES);
    glColor3f(1.0f, 0.0f, 0.0f);
    glVertex2f(-1.0f, -1.0f);
    glVertex2f(1.0f, -1.0f);
    glVertex2f(-1.0f, 1.0f);
    glEnd();
}

int main(void)
{
    PIXELFORMATDESCRIPTOR pfd;
    WNDCLASSA wc;
    HWND hwnd;
    HDC dc;
    HGLRC ctx;
    RECT rc;
    POINT lo, hi;
    int format, w, h;
    unsigned char px[4];
    const char *renderer;
    char line[200];

    wm_fd = (int)linux3(LINUX_OPEN, (LONGLONG)(INT_PTR)"/dev/wm", 2, 0);

    memset(&wc, 0, sizeof(wc));
    wc.style         = CS_OWNDC;
    wc.lpfnWndProc   = DefWindowProcA;
    wc.hInstance     = GetModuleHandleA(NULL);
    wc.lpszClassName = "NovarisGL";
    RegisterClassA(&wc);

    hwnd = CreateWindowExA(0, "NovarisGL", "OpenGL", WS_OVERLAPPEDWINDOW,
                           200, 150, 320, 260, NULL, NULL, wc.hInstance, NULL);
    check("a window", hwnd != NULL);
    if (!hwnd) return failed;
    ShowWindow(hwnd, SW_SHOWNORMAL);
    UpdateWindow(hwnd);
    pump(300);

    dc = GetDC(hwnd);
    memset(&pfd, 0, sizeof(pfd));
    pfd.nSize      = sizeof(pfd);
    pfd.nVersion   = 1;
    pfd.dwFlags    = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    pfd.iLayerType = PFD_MAIN_PLANE;
    format = ChoosePixelFormat(dc, &pfd);
    snprintf(line, sizeof(line), "     pixel format %d", format);
    puts(line);
    check("a pixel format with OpenGL in it", format > 0);
    check("set on the window", format > 0 && SetPixelFormat(dc, format, &pfd));
    ctx = wglCreateContext(dc);
    check("a WGL context", ctx != NULL);
    check("made current", ctx && wglMakeCurrent(dc, ctx));
    if (failed) return failed;

    renderer = (const char *)glGetString(GL_RENDERER);
    snprintf(line, sizeof(line), "     renderer \"%.8s\", version %.3s", renderer ? renderer : "(none)",
             (const char *)glGetString(GL_VERSION));
    puts(line);
    fflush(stdout);
    check("the renderer is Mesa's llvmpipe", renderer && !strncmp(renderer, "llvmpipe", 8));

    GetClientRect(hwnd, &rc);
    w = rc.right - rc.left;
    h = rc.bottom - rc.top;
    draw(w, h);
    glFinish();
    glReadPixels(4, 4, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    snprintf(line, sizeof(line), "     glReadPixels (4,4) = %02x%02x%02x, glGetError %#x",
             px[0], px[1], px[2], glGetError());
    puts(line);
    fflush(stdout);
    check("GL itself reads the triangle back", px[0] == 0xff && px[1] == 0 && px[2] == 0);

    /* Twice, so the frame on the screen is the second one: a swap
     * whose copy into the window had not happened yet would still pass
     * after a single frame if the surface already held the first. */
    SwapBuffers(dc);
    draw(w, h);
    SwapBuffers(dc);
    pump(500);

    lo.x = rc.left + 6;  lo.y = rc.bottom - 6;    /* bottom-left: in the triangle */
    hi.x = rc.right - 6; hi.y = rc.top + 6;       /* top-right: the clear colour */
    ClientToScreen(hwnd, &lo);
    ClientToScreen(hwnd, &hi);
    snprintf(line, sizeof(line), "     on screen: (%ld,%ld) #%06x, (%ld,%ld) #%06x",
             lo.x, lo.y, peek(lo.x, lo.y), hi.x, hi.y, peek(hi.x, hi.y));
    puts(line);
    fflush(stdout);
    check("SwapBuffers puts the triangle on the screen", peek(lo.x, lo.y) == 0xff0000);
    check("and the clear colour around it",
          (peek(hi.x, hi.y) & 0xfcfcfc) == (0x4080bf & 0xfcfcfc));

    wglMakeCurrent(NULL, NULL);
    wglDeleteContext(ctx);
    ReleaseDC(hwnd, dc);
    DestroyWindow(hwnd);
    if (wm_fd >= 0) linux3(LINUX_CLOSE, wm_fd, 0, 0);

    if (failed) printf("winegl64: check %d did not hold\n", failed);
    else puts("winegl64: OpenGL drew into a window, through Mesa");
    fflush(stdout);
    return failed;
}
