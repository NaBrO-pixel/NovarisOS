/* egl64.c - OpenGL with no display at all (Milestone 110).
 *
 * Mesa's surfaceless EGL platform and its llvmpipe renderer: a context
 * and a pbuffer with no window system under them, a triangle drawn into
 * it, and the pixels read back. This is the path Wine 11's win32u takes
 * for a display driver that has no OpenGL of its own - winenovaris.drv -
 * so it is the floor every Windows OpenGL program here stands on, tested
 * without Wine in the way.
 *
 * llvmpipe is a real workload for a kernel: it JIT-compiles shaders with
 * LLVM into executable memory, rasterizes on a pool of threads, and
 * reads /proc and /sys to decide how many. Run on the Linux host and on
 * NovarisOS, and the two must agree.
 *
 * Every GL entry point comes through eglGetProcAddress, so this links
 * against libEGL alone - as Wine does.
 *
 * Exits 110 when everything held.
 */

#include <stdio.h>
#include <string.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

#define GL_COLOR_BUFFER_BIT 0x00004000
#define GL_TRIANGLES        0x0004
#define GL_RGBA             0x1908
#define GL_UNSIGNED_BYTE    0x1401
#define GL_RENDERER         0x1F01
#define GL_VERSION          0x1F02

typedef void (*clearcolor_fn)(float, float, float, float);
typedef void (*clear_fn)(unsigned);
typedef void (*viewport_fn)(int, int, int, int);
typedef void (*begin_fn)(unsigned);
typedef void (*end_fn)(void);
typedef void (*color3f_fn)(float, float, float);
typedef void (*vertex2f_fn)(float, float);
typedef void (*finish_fn)(void);
typedef void (*readpixels_fn)(int, int, int, int, unsigned, unsigned, void *);
typedef const unsigned char *(*getstring_fn)(unsigned);

static int failures;

static void ok(const char *what, int cond)
{
    printf("%-4s egl: %s\n", cond ? "ok" : "FAIL", what);
    if (!cond) failures++;
}

#define W 64
#define H 64

int main(void)
{
    PFNEGLGETPLATFORMDISPLAYEXTPROC get_display;
    EGLDisplay dpy;
    EGLConfig config;
    EGLSurface surface;
    EGLContext ctx;
    EGLint major = 0, minor = 0, n = 0;
    static const EGLint config_attribs[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
        EGL_NONE
    };
    static const EGLint pbuffer_attribs[] = { EGL_WIDTH, W, EGL_HEIGHT, H, EGL_NONE };
    static unsigned char px[W * H * 4];
    const char *renderer;

    setvbuf(stdout, NULL, _IONBF, 0);

    get_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    ok("eglGetPlatformDisplayEXT exists", get_display != NULL);
    if (!get_display) return 1;
    dpy = get_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
    ok("a surfaceless display", dpy != EGL_NO_DISPLAY);
    ok("EGL initializes", eglInitialize(dpy, &major, &minor) && major >= 1);
    ok("desktop OpenGL can be bound", eglBindAPI(EGL_OPENGL_API));
    ok("a config with a pbuffer", eglChooseConfig(dpy, config_attribs, &config, 1, &n) && n == 1);
    if (failures) return 1;
    surface = eglCreatePbufferSurface(dpy, config, pbuffer_attribs);
    ok("a 64x64 pbuffer", surface != EGL_NO_SURFACE);
    ctx = eglCreateContext(dpy, config, EGL_NO_CONTEXT, NULL);
    ok("a context", ctx != EGL_NO_CONTEXT);
    ok("made current", eglMakeCurrent(dpy, surface, surface, ctx));
    if (failures) return 1;

    {
        clearcolor_fn ClearColor = (clearcolor_fn)eglGetProcAddress("glClearColor");
        clear_fn Clear = (clear_fn)eglGetProcAddress("glClear");
        viewport_fn Viewport = (viewport_fn)eglGetProcAddress("glViewport");
        begin_fn Begin = (begin_fn)eglGetProcAddress("glBegin");
        end_fn End = (end_fn)eglGetProcAddress("glEnd");
        color3f_fn Color3f = (color3f_fn)eglGetProcAddress("glColor3f");
        vertex2f_fn Vertex2f = (vertex2f_fn)eglGetProcAddress("glVertex2f");
        finish_fn Finish = (finish_fn)eglGetProcAddress("glFinish");
        readpixels_fn ReadPixels = (readpixels_fn)eglGetProcAddress("glReadPixels");
        getstring_fn GetString = (getstring_fn)eglGetProcAddress("glGetString");

        renderer = (const char *)GetString(GL_RENDERER);
        /* The renderer's name carries the LLVM version and the CPU's
         * vector width, which differ between machines; only the part
         * that names the renderer is printed, so host and guest agree. */
        ok("the renderer is llvmpipe", renderer && strncmp(renderer, "llvmpipe", 8) == 0);
        printf("     version %.3s\n", (const char *)GetString(GL_VERSION));

        Viewport(0, 0, W, H);
        ClearColor(0.25f, 0.5f, 0.75f, 1.0f);
        Clear(GL_COLOR_BUFFER_BIT);
        /* The lower-left half of the square, red. */
        Begin(GL_TRIANGLES);
        Color3f(1.0f, 0.0f, 0.0f);
        Vertex2f(-1.0f, -1.0f);
        Vertex2f(1.0f, -1.0f);
        Vertex2f(-1.0f, 1.0f);
        End();
        Finish();
        memset(px, 0, sizeof(px));
        ReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
    }

    {
        const unsigned char *lo = &px[(4 * W + 4) * 4];            /* (4,4) */
        const unsigned char *hi = &px[((H - 5) * W + (W - 5)) * 4]; /* (59,59) */
        printf("     pixel (4,4) = %02x%02x%02x, (59,59) = %02x%02x%02x\n",
               lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]);
        ok("the triangle is drawn", lo[0] == 0xff && lo[1] == 0 && lo[2] == 0);
        ok("and the clear colour is outside it",
           hi[0] >= 0x3f && hi[0] <= 0x40 && hi[1] >= 0x7f && hi[1] <= 0x80 &&
           hi[2] >= 0xbf && hi[2] <= 0xc0);
    }

    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(dpy, ctx);
    eglDestroySurface(dpy, surface);
    ok("EGL terminates", eglTerminate(dpy));

    printf("egl: %s\n", failures ? "FAILED" : "all held");
    return failures ? 1 : 110;
}
