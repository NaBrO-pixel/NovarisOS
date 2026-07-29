/* gui_app.c - a Windows GUI-subsystem program.
 *
 * Built with -mwindows, so its PE header says IMAGE_SUBSYSTEM_WINDOWS_GUI
 * and its entry point is WinMainCRTStartup rather than mainCRTStartup.
 * Novaris has no window manager (ROADMAP.md Milestone 7), which makes
 * this a test of the *other* half of compatibility: what happens to a
 * program that asks for something this OS genuinely cannot do.
 *
 * MessageBoxA is emulated for real - drawn as a box on the console - so
 * that part works. CreateWindowEx cannot be, and reports failure; the
 * point is that the program takes its own error path and exits cleanly,
 * rather than hanging or faulting. */

#include <windows.h>

int WINAPI WinMain(HINSTANCE instance, HINSTANCE previous,
                   LPSTR command_line, int show) {
    (void)previous;
    (void)command_line;

    MessageBoxA(NULL, "A GUI program's message box, on a text console.",
                "gui_app.exe", MB_OK | MB_ICONINFORMATION);

    /* Now ask for something Novaris really can't provide, and handle the
     * failure the way any careful Windows program would. */
    HWND window = CreateWindowExA(0, "STATIC", "Novaris", WS_OVERLAPPEDWINDOW,
                                  CW_USEDEFAULT, CW_USEDEFAULT, 640, 480,
                                  NULL, NULL, instance, NULL);
    if (!window) {
        MessageBoxA(NULL, "CreateWindowEx failed, as expected. Exiting.",
                    "gui_app.exe", MB_OK | MB_ICONERROR);
        return 1;
    }

    ShowWindow(window, show);

    MSG message;
    while (GetMessageA(&message, NULL, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageA(&message);
    }
    return 0;
}
