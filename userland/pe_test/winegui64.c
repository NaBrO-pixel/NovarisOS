/* winegui64.c - a Windows program that really does make a window.
 *
 * The difference between this and pe_test/gui_app.c is the whole point.
 * gui_app.c was written for this kernel's own PE loader, and it expects
 * CreateWindowEx to fail: it is a test of what a program does when the
 * OS cannot help it. This one runs under Wine, on a kernel that now has
 * a display driver, and expects CreateWindowEx to *work*.
 *
 * Console subsystem on purpose, though it is a GUI program in every
 * other sense. A IMAGE_SUBSYSTEM_WINDOWS_GUI binary has nowhere to
 * write, and everything else in this tree reports by writing to the
 * serial port; keeping stdout means the run says what happened in the
 * same place as every other layer. It still registers a class, creates
 * an overlapped window, shows it, and pumps messages - which is the
 * sequence that has to work before anything with a user interface does.
 *
 * The exit code is the assertion. 0 means every step below succeeded;
 * anything else names the step that did not, so a run that fails says
 * where without needing the log parsed.
 */

#include <windows.h>
#include <stdio.h>

#define EXIT_OK              0
#define EXIT_NO_CLASS        2
#define EXIT_NO_WINDOW       3
#define EXIT_NO_DC           4
#define EXIT_NO_PUMP         5
#define EXIT_BAD_RECT        6

/* One write per line, not one per byte.
 *
 * setvbuf(_IONBF) was the obvious thing and the wrong one: msvcrt honours
 * it exactly, so every character became its own write(2), and with the
 * kernel's syscall trace on, each one arrived in the serial log on its
 * own line between two trace records. The program's output was all
 * there and unreadable - "ok   the window class registered" spelled one
 * letter per line down a hundred lines of log. Buffered, with an
 * explicit flush per line, each line is a single write. */
static void say( const char *s )
{
    fputs( s, stdout );
    fputc( '\n', stdout );
    fflush( stdout );
}

static int painted;

static LRESULT CALLBACK wndproc( HWND hwnd, UINT msg, WPARAM wp, LPARAM lp )
{
    switch (msg)
    {
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint( hwnd, &ps );
        if (dc)
        {
            RECT r;
            GetClientRect( hwnd, &r );
            FillRect( dc, &r, (HBRUSH)(COLOR_WINDOW + 1) );
            EndPaint( hwnd, &ps );
            painted++;
        }
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage( 0 );
        return 0;
    }
    return DefWindowProcA( hwnd, msg, wp, lp );
}

int main( void )
{
    WNDCLASSA wc;
    HWND hwnd;
    HDC dc;
    MSG msg;
    int pumped = 0, i;
    RECT rect;


    memset( &wc, 0, sizeof(wc) );
    wc.lpfnWndProc   = wndproc;
    wc.hInstance     = GetModuleHandleA( NULL );
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = "NovarisWineGui";

    if (!RegisterClassA( &wc ))
    {
        printf( "winegui64: RegisterClass failed, %lu\n", GetLastError() );
        fflush( stdout );
        return EXIT_NO_CLASS;
    }
    say( "ok   the window class registered" );

    hwnd = CreateWindowExA( 0, "NovarisWineGui", "Novaris",
                            WS_OVERLAPPEDWINDOW, 100, 100, 400, 300,
                            NULL, NULL, wc.hInstance, NULL );
    if (!hwnd)
    {
        printf( "winegui64: CreateWindowEx failed, %lu\n", GetLastError() );
        fflush( stdout );
        return EXIT_NO_WINDOW;
    }
    say( "ok   a window exists" );

    ShowWindow( hwnd, SW_SHOWNORMAL );
    UpdateWindow( hwnd );
    say( "ok   it was shown" );

    /* A device context for it is what every drawing operation starts
     * from, and getting one is a different path through the driver than
     * creating the window was. */
    dc = GetDC( hwnd );
    if (!dc)
    {
        printf( "winegui64: GetDC failed, %lu\n", GetLastError() );
        fflush( stdout );
        return EXIT_NO_DC;
    }
    ReleaseDC( hwnd, dc );
    say( "ok   it has a device context" );

    /* The client rectangle, checked rather than printed.
     *
     * Printing it was not enough: the first run of this program reported
     * "its client area is 6750416x0" for a window asked to be 400x300,
     * and called that a pass because GetClientRect had returned TRUE. A
     * window whose size nothing can read is not a working window, so the
     * number is now compared against the one that was asked for. The
     * bounds are loose on purpose - a window manager is entitled to
     * decide the frame, and this one has no title bar or borders yet -
     * but they do exclude zero and they do exclude nonsense. */
    /* A sentinel first, so "did not write" and "wrote nonsense" are
     * different answers. The rect was an uninitialised local the first
     * time and read 6750416x0, which could have been either. */
    rect.left = rect.top = rect.right = rect.bottom = 0x5A5A5A5A;
    if (!GetClientRect( hwnd, &rect ))
    {
        printf( "winegui64: GetClientRect failed, %lu\n", GetLastError() );
        fflush( stdout );
        return EXIT_BAD_RECT;
    }
    if (rect.left == 0x5A5A5A5A && rect.top == 0x5A5A5A5A &&
        rect.right == 0x5A5A5A5A && rect.bottom == 0x5A5A5A5A)
    {
        say( "bad  GetClientRect returned TRUE and wrote nothing" );
        return EXIT_BAD_RECT;
    }
    printf( "     client rect  = (%ld,%ld)-(%ld,%ld)\n",
            rect.left, rect.top, rect.right, rect.bottom );
    fflush( stdout );
    {
        RECT wr;
        wr.left = wr.top = wr.right = wr.bottom = 0x5A5A5A5A;
        if (GetWindowRect( hwnd, &wr ))
            printf( "     window rect  = (%ld,%ld)-(%ld,%ld)\n",
                    wr.left, wr.top, wr.right, wr.bottom );
        else
            printf( "     GetWindowRect failed, %lu\n", GetLastError() );
        fflush( stdout );
    }
    /* The metrics the frame is computed from, and a window that has no
     * frame at all.
     *
     * WS_OVERLAPPEDWINDOW's rect is the client area grown by the caption
     * and border, and those come from system metrics that are derived
     * from the caption *font*. This Wine reports "cannot find the
     * FreeType font library" on the way up, so the question is whether
     * the frame arithmetic is being done with numbers no font supplied.
     * A WS_POPUP window has no caption and no border, so its rect is the
     * one that was asked for and nothing else - if that one is right and
     * the overlapped one is wrong, the frame is where the nonsense
     * enters. */
    printf( "     metrics: caption=%d xframe=%d yframe=%d border=%d\n",
            GetSystemMetrics( SM_CYCAPTION ), GetSystemMetrics( SM_CXFRAME ),
            GetSystemMetrics( SM_CYFRAME ), GetSystemMetrics( SM_CXBORDER ) );
    fflush( stdout );
    {
        HWND pop = CreateWindowExA( 0, "NovarisWineGui", "Popup", WS_POPUP,
                                    10, 10, 200, 150, NULL, NULL,
                                    wc.hInstance, NULL );
        RECT pr;
        pr.left = pr.top = pr.right = pr.bottom = 0x5A5A5A5A;
        if (pop && GetWindowRect( pop, &pr ))
            printf( "     popup  rect  = (%ld,%ld)-(%ld,%ld)  [asked 10,10 200x150]\n",
                    pr.left, pr.top, pr.right, pr.bottom );
        else
            printf( "     popup window failed, %lu\n", GetLastError() );
        fflush( stdout );
        if (pop) DestroyWindow( pop );
    }
    {
        HWND desk = GetDesktopWindow();
        RECT dr;
        dr.left = dr.top = dr.right = dr.bottom = 0x5A5A5A5A;
        if (desk && GetClientRect( desk, &dr ))
            printf( "     desktop rect = (%ld,%ld)-(%ld,%ld), visible=%d\n",
                    dr.left, dr.top, dr.right, dr.bottom,
                    IsWindowVisible( hwnd ) );
        else
            printf( "     no desktop rect (desk=%p)\n", (void *)desk );
        fflush( stdout );
    }
    {
        long w = rect.right - rect.left, h = rect.bottom - rect.top;
        /* "bad", not "FAIL": the test target greps the whole serial log
         * for FAIL and stops the suite on it, and this program's verdict
         * belongs in its exit code rather than in a word that fails
         * twenty-one unrelated differentials. */
        printf( "%s  its client area is %ldx%ld\n",
                (w > 0 && w <= 400 && h > 0 && h <= 300) ? "ok " : "bad", w, h );
        fflush( stdout );
        if (!(w > 0 && w <= 400 && h > 0 && h <= 300)) return EXIT_BAD_RECT;
    }

    /* Pump what is queued. Not a real message loop - there is nobody to
     * close the window - so it drains what is there and stops. */
    for (i = 0; i < 200; i++)
    {
        if (!PeekMessageA( &msg, NULL, 0, 0, PM_REMOVE )) break;
        TranslateMessage( &msg );
        DispatchMessageA( &msg );
        pumped++;
    }
    printf( "ok   the message loop ran (%d message%s, %d paint%s)\n",
            pumped, pumped == 1 ? "" : "s", painted, painted == 1 ? "" : "s" );
    fflush( stdout );

    DestroyWindow( hwnd );
    say( "ok   it was destroyed" );

    say( "winegui64: a Windows window was created and drawn" );
    return EXIT_OK;
}
