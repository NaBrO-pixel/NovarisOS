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

    setvbuf( stdout, NULL, _IONBF, 0 );

    memset( &wc, 0, sizeof(wc) );
    wc.lpfnWndProc   = wndproc;
    wc.hInstance     = GetModuleHandleA( NULL );
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = "NovarisWineGui";

    if (!RegisterClassA( &wc ))
    {
        printf( "winegui64: RegisterClass failed, %lu\n", GetLastError() );
        return EXIT_NO_CLASS;
    }
    printf( "ok   the window class registered\n" );

    hwnd = CreateWindowExA( 0, "NovarisWineGui", "Novaris",
                            WS_OVERLAPPEDWINDOW, 100, 100, 400, 300,
                            NULL, NULL, wc.hInstance, NULL );
    if (!hwnd)
    {
        printf( "winegui64: CreateWindowEx failed, %lu\n", GetLastError() );
        return EXIT_NO_WINDOW;
    }
    printf( "ok   a window exists\n" );

    ShowWindow( hwnd, SW_SHOWNORMAL );
    UpdateWindow( hwnd );
    printf( "ok   it was shown\n" );

    /* A device context for it is what every drawing operation starts
     * from, and getting one is a different path through the driver than
     * creating the window was. */
    dc = GetDC( hwnd );
    if (!dc)
    {
        printf( "winegui64: GetDC failed, %lu\n", GetLastError() );
        return EXIT_NO_DC;
    }
    ReleaseDC( hwnd, dc );
    printf( "ok   it has a device context\n" );

    if (GetClientRect( hwnd, &rect ))
        printf( "ok   its client area is %ldx%ld\n",
                rect.right - rect.left, rect.bottom - rect.top );

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

    DestroyWindow( hwnd );
    printf( "ok   it was destroyed\n" );

    printf( "winegui64: a Windows window was created and drawn\n" );
    return EXIT_OK;
}
