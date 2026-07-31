/* win32_user32.c - user32, gdi32, advapi32, shell32, and the catch-all
 * `misc` module.
 *
 * Novaris has a framebuffer and a mouse cursor but no window manager
 * (ROADMAP.md Milestone 7 is explicit that the "terminal window" is
 * static chrome), so there is nothing here for a GUI program to actually
 * draw into. What this file does instead is make GUI-flavored calls fail
 * *usefully*:
 *
 *   - MessageBox is rendered as a box on the console, which is a genuine
 *     implementation of the one GUI call console-adjacent programs
 *     actually make.
 *   - Window creation reports failure rather than returning a handle the
 *     program would then draw into and get nothing from. A program that
 *     checks CreateWindowEx's return - most do - exits with its own
 *     error message, which is far more informative than a black screen.
 *   - GetMessage returns 0, which means WM_QUIT: a standard message loop
 *     terminates cleanly instead of spinning forever.
 *
 * The `misc` module at the bottom is the resolver's last stop before
 * declaring an import missing (see resolve_symbol in win32.c). It holds
 * the few symbols that turn up under many different DLL names. */

#include "win32_internal.h"
#include "kstring.h"
#include "console.h"

#define ARG(n) (call->args[n])

/* --- user32 ------------------------------------------------------------ */

/* MessageBoxA(owner, text, caption, type) - drawn as a console box.
 * Returns IDOK (1), which is what a program showing an informational
 * message expects. */
static uint32_t u32_MessageBoxA(win32_call_t* call) {
    const char* text = (const char*)ARG(1);
    const char* caption = (const char*)ARG(2);
    if (!text) text = "";
    if (!caption) caption = "Message";

    uint32_t width = kstrlen(text);
    uint32_t caption_len = kstrlen(caption);
    if (caption_len > width) width = caption_len;
    if (width > 60) width = 60;

    terminal_writestring("\n  +");
    for (uint32_t i = 0; i < width + 2; i++) terminal_writestring("-");
    terminal_writestring("+\n  | ");
    terminal_writestring_color(caption, VGA_COLOR_WHITE);
    for (uint32_t i = caption_len; i < width; i++) terminal_writestring(" ");
    terminal_writestring(" |\n  | ");
    terminal_writestring(text);
    for (uint32_t i = kstrlen(text); i < width; i++) terminal_writestring(" ");
    terminal_writestring(" |\n  +");
    for (uint32_t i = 0; i < width + 2; i++) terminal_writestring("-");
    terminal_writestring("+\n");

    return 1; /* IDOK */
}

static uint32_t u32_MessageBoxW(win32_call_t* call) {
    /* Narrow both strings and hand off. Two separate scratch buffers are
     * needed because w32_wide_arg() reuses one. */
    char text[200];
    char caption[80];
    w32_wide_to_ascii((const uint16_t*)ARG(1), text, sizeof(text));
    w32_wide_to_ascii((const uint16_t*)ARG(2), caption, sizeof(caption));

    uint32_t narrowed[4] = { ARG(0), (uint32_t)text, (uint32_t)caption, ARG(3) };
    /* The synthesized call keeps the outer one's ring-3 esp: it is the
     * same trap, just with narrowed arguments, so anything downstream that
     * needs the caller's stack (a callback, a fault report) should see
     * the real one rather than 0. */
    win32_call_t inner = { call->regs, narrowed, call->useresp };
    return u32_MessageBoxA(&inner);
}

/* wsprintfA(buffer, format, ...) - user32's own printf, cdecl despite
 * living in a stdcall DLL. */
static uint32_t u32_wsprintfA(win32_call_t* call) {
    char* buffer = (char*)ARG(0);
    const char* fmt = (const char*)ARG(1);
    if (!buffer) return 0;
    return w32_format(buffer, 1024, fmt, &call->args[2]);
}

static uint32_t u32_CreateWindowExA(win32_call_t* call) {
    (void)call;
    w32_report("CreateWindowEx: Novaris has no window manager, so this fails.");
    w32_set_error(ERROR_CALL_NOT_IMPLEMENTED);
    return 0;
}

static uint32_t u32_GetSystemMetrics(win32_call_t* call) {
    switch (ARG(0)) {
        case 0: return 1280; /* SM_CXSCREEN */
        case 1: return 800;  /* SM_CYSCREEN */
        default: return 0;
    }
}

static uint32_t u32_CharUpperA(win32_call_t* call) {
    uint32_t arg = ARG(0);
    /* A value under 0x10000 is a single character, not a pointer - the
     * same MAKEINTRESOURCE convention GetProcAddress uses. */
    if (arg < 0x10000) {
        return (arg >= 'a' && arg <= 'z') ? arg - 32 : arg;
    }
    for (char* s = (char*)arg; *s; s++) {
        if (*s >= 'a' && *s <= 'z') *s = (char)(*s - 32);
    }
    return arg;
}

static const win32_export_t user32_exports[] = {
    WEXP("MessageBoxA", 4, u32_MessageBoxA),
    WEXP("MessageBoxW", 4, u32_MessageBoxW),
    WEXP("MessageBoxExA", 5, u32_MessageBoxA),
    WEXP_C("wsprintfA", u32_wsprintfA),
    WEXP_C("wsprintfW", u32_wsprintfA),
    WEXP("GetSystemMetrics", 1, u32_GetSystemMetrics),
    WEXP("CharUpperA", 1, u32_CharUpperA),
    WEXP("CreateWindowExA", 12, u32_CreateWindowExA),
    WEXP("CreateWindowExW", 12, u32_CreateWindowExA),

    WSTUB("RegisterClassA", 1, 0),
    WSTUB("RegisterClassExA", 1, 0),
    WSTUB("RegisterClassW", 1, 0),
    WSTUB("UnregisterClassA", 2, 1),
    WSTUB("DestroyWindow", 1, 1),
    WSTUB("ShowWindow", 2, 0),
    WSTUB("UpdateWindow", 1, 0),
    WSTUB("GetMessageA", 4, 0),   /* 0 == WM_QUIT: message loops exit cleanly */
    WSTUB("GetMessageW", 4, 0),
    WSTUB("PeekMessageA", 5, 0),
    WSTUB("TranslateMessage", 1, 0),
    WSTUB("DispatchMessageA", 1, 0),
    WSTUB("DefWindowProcA", 4, 0),
    WSTUB("DefWindowProcW", 4, 0),
    WSTUB("PostQuitMessage", 1, 0),
    WSTUB("PostMessageA", 4, 0),
    WSTUB("SendMessageA", 4, 0),
    WSTUB("LoadIconA", 2, 0),
    WSTUB("LoadCursorA", 2, 0),
    WSTUB("LoadImageA", 6, 0),
    WSTUB("LoadStringA", 4, 0),
    WSTUB("GetDC", 1, 0),
    WSTUB("ReleaseDC", 2, 1),
    WSTUB("BeginPaint", 2, 0),
    WSTUB("EndPaint", 2, 1),
    WSTUB("InvalidateRect", 3, 1),
    WSTUB("SetWindowTextA", 2, 1),
    WSTUB("GetWindowTextA", 3, 0),
    WSTUB("MessageBeep", 1, 1),
    WSTUB("GetDesktopWindow", 0, 0),
    WSTUB("GetForegroundWindow", 0, 0),
    WSTUB("SetForegroundWindow", 1, 0),
    WSTUB("GetClientRect", 2, 1),
    WSTUB("GetWindowRect", 2, 1),
    WSTUB("MoveWindow", 6, 1),
    WSTUB("SetTimer", 4, 0),
    WSTUB("KillTimer", 2, 1),
    WSTUB("ExitWindowsEx", 2, 0),
    WSTUB("CharNextA", 1, 0),
    WSTUB("wvsprintfA", 3, 0),
};

const win32_module_t win32_module_user32 = {
    "user32.dll", user32_exports,
    sizeof(user32_exports) / sizeof(user32_exports[0]), 0
};

/* --- gdi32 ------------------------------------------------------------- */

/* No drawing surface exists for a program to render into, so every one of
 * these reports failure. They are present rather than absent so that an
 * import of gdi32 doesn't turn into a missing-import warning for a
 * program that merely links against it without drawing. */
static const win32_export_t gdi32_exports[] = {
    WSTUB("CreateCompatibleDC", 1, 0),
    WSTUB("CreateCompatibleBitmap", 3, 0),
    WSTUB("DeleteDC", 1, 1),
    WSTUB("DeleteObject", 1, 1),
    WSTUB("SelectObject", 2, 0),
    WSTUB("GetStockObject", 1, 0),
    WSTUB("SetTextColor", 2, 0),
    WSTUB("SetBkColor", 2, 0),
    WSTUB("SetBkMode", 2, 0),
    WSTUB("TextOutA", 5, 0),
    WSTUB("BitBlt", 9, 0),
    WSTUB("Rectangle", 5, 0),
    WSTUB("MoveToEx", 4, 0),
    WSTUB("LineTo", 3, 0),
    WSTUB("CreatePen", 3, 0),
    WSTUB("CreateSolidBrush", 1, 0),
    WSTUB("CreateFontA", 14, 0),
    WSTUB("GetDeviceCaps", 2, 0),
};

const win32_module_t win32_module_gdi32 = {
    "gdi32.dll", gdi32_exports,
    sizeof(gdi32_exports) / sizeof(gdi32_exports[0]), 0
};

/* --- advapi32 ---------------------------------------------------------- */

/* There is no registry. Every Reg* call returns ERROR_FILE_NOT_FOUND (2),
 * which is a result callers are required to handle and routinely do -
 * unlike a success return with an unusable key handle. */
static const win32_export_t advapi32_exports[] = {
    WSTUB("RegOpenKeyExA", 5, 2),
    WSTUB("RegOpenKeyA", 3, 2),
    WSTUB("RegQueryValueExA", 6, 2),
    WSTUB("RegCloseKey", 1, 0),
    WSTUB("RegCreateKeyExA", 9, 2),
    WSTUB("RegSetValueExA", 6, 2),
    WSTUB("RegDeleteKeyA", 2, 2),
    WSTUB("RegEnumKeyExA", 8, 2),
    WSTUB("GetUserNameA", 2, 0),
    WSTUB("OpenProcessToken", 3, 0),
    WSTUB("CryptAcquireContextA", 5, 0),
    WSTUB("CryptGenRandom", 3, 0),
    WSTUB("CryptReleaseContext", 2, 1),
};

const win32_module_t win32_module_advapi32 = {
    "advapi32.dll", advapi32_exports,
    sizeof(advapi32_exports) / sizeof(advapi32_exports[0]), 0
};

/* --- shell32 ----------------------------------------------------------- */

static const win32_export_t shell32_exports[] = {
    WSTUB("ShellExecuteA", 6, 0),
    WSTUB("SHGetFolderPathA", 5, 0x80004005u), /* E_FAIL */
    WSTUB("SHGetSpecialFolderPathA", 4, 0),
    WSTUB("CommandLineToArgvW", 2, 0),
    WSTUB("ExtractIconA", 3, 0),
    WSTUB("Shell_NotifyIconA", 2, 0),
};

const win32_module_t win32_module_shell32 = {
    "shell32.dll", shell32_exports,
    sizeof(shell32_exports) / sizeof(shell32_exports[0]), 0
};

/* --- misc -------------------------------------------------------------- */

/* Symbols that turn up imported from DLLs this OS doesn't model at all
 * (ole32, comctl32, oleaut32, winmm, ws2_32, version, ...). Consulted
 * only after the named module has been tried, so a real implementation
 * always wins - see resolve_symbol() in win32.c. */
static const win32_export_t misc_exports[] = {
    WSTUB("CoInitialize", 1, 0),
    WSTUB("CoInitializeEx", 2, 0),
    WSTUB("CoUninitialize", 0, 0),
    WSTUB("CoCreateInstance", 5, 0x80004005u),
    WSTUB("CoTaskMemAlloc", 1, 0),
    WSTUB("CoTaskMemFree", 1, 0),
    WSTUB("OleInitialize", 1, 0),
    WSTUB("OleUninitialize", 0, 0),
    WSTUB("InitCommonControls", 0, 0),
    WSTUB("InitCommonControlsEx", 1, 1),
    WSTUB("timeGetTime", 0, 0),
    WSTUB("PlaySoundA", 3, 0),
    WSTUB("WSAStartup", 2, 10093),   /* WSANOTINITIALISED: no network stack */
    WSTUB("WSACleanup", 0, 0),
    WSTUB("GetFileVersionInfoA", 4, 0),
    WSTUB("GetFileVersionInfoSizeA", 2, 0),
    WSTUB("VerQueryValueA", 4, 0),
    WSTUB("SysAllocString", 1, 0),
    WSTUB("SysFreeString", 1, 0),
    WSTUB("VariantInit", 1, 0),
    WSTUB("VariantClear", 1, 0),
    WSTUB("PathFindFileNameA", 1, 0),
    WSTUB("StrCmpNIA", 3, 0),
};

const win32_module_t win32_module_misc = {
    "misc", misc_exports,
    sizeof(misc_exports) / sizeof(misc_exports[0]), 0
};
