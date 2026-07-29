/* win32_api.c - a Windows program that calls the Win32 API directly,
 * with no C runtime involved in the interesting parts.
 *
 * hello_win.c proves the C runtime side of the emulation; this one proves
 * the kernel32/user32 side. It uses the stdcall API surface a Windows
 * program would use for console I/O, memory, timing, module handles and
 * file access, and reads back what it gets rather than just calling and
 * ignoring the results - so a stubbed-out API shows up as visibly wrong
 * output rather than silently passing.
 *
 * Built with stock i686-w64-mingw32-gcc against the real windows.h. */

#include <windows.h>

static void out(const char* text) {
    DWORD written;
    WriteConsoleA(GetStdHandle(STD_OUTPUT_HANDLE), text,
                  (DWORD)lstrlenA(text), &written, NULL);
}

static void out_num(const char* label, DWORD value) {
    char buffer[128];
    wsprintfA(buffer, "%s%lu\r\n", label, value);
    out(buffer);
}

static void out_hex(const char* label, DWORD value) {
    char buffer[128];
    wsprintfA(buffer, "%s0x%08lx\r\n", label, value);
    out(buffer);
}

int WINAPI WinMainCRTStartup(void);

int main(void) {
    SYSTEM_INFO info;
    SYSTEMTIME now;
    MEMORYSTATUS memory;

    out("Win32 API test - talking to kernel32 and user32 directly.\r\n\r\n");

    out_hex("GetModuleHandle(NULL):  ", (DWORD)(UINT_PTR)GetModuleHandleA(NULL));
    out_hex("kernel32.dll handle:    ", (DWORD)(UINT_PTR)GetModuleHandleA("kernel32.dll"));
    out_hex("GetProcAddress(Sleep):  ",
            (DWORD)(UINT_PTR)GetProcAddress(GetModuleHandleA("kernel32.dll"), "Sleep"));

    out("command line:           ");
    out(GetCommandLineA());
    out("\r\n");

    GetSystemInfo(&info);
    out_num("page size:              ", info.dwPageSize);
    out_num("processors:             ", info.dwNumberOfProcessors);

    GlobalMemoryStatus(&memory);
    out_num("physical memory (KB):   ", (DWORD)(memory.dwTotalPhys / 1024));
    out_num("available (KB):         ", (DWORD)(memory.dwAvailPhys / 1024));

    GetSystemTime(&now);
    {
        char buffer[128];
        wsprintfA(buffer, "system time:            %04d-%02d-%02d %02d:%02d:%02d UTC\r\n",
                  now.wYear, now.wMonth, now.wDay,
                  now.wHour, now.wMinute, now.wSecond);
        out(buffer);
    }

    /* The heap, read back rather than just allocated. */
    {
        char* block = (char*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 256);
        if (block) {
            lstrcpyA(block, "HeapAlloc round-trip");
            out("heap:                   ");
            out(block);
            out("\r\n");
            HeapFree(GetProcessHeap(), 0, block);
        } else {
            out("heap:                   HeapAlloc FAILED\r\n");
        }
    }

    /* VirtualAlloc, then actually write through the pointer. */
    {
        DWORD* pages = (DWORD*)VirtualAlloc(NULL, 8192, MEM_COMMIT, PAGE_READWRITE);
        if (pages) {
            pages[0] = 0xCAFEBABE;
            pages[2047] = 0xDEADBEEF;
            out_hex("VirtualAlloc first dword: ", pages[0]);
            out_hex("VirtualAlloc last dword:  ", pages[2047]);
            VirtualFree(pages, 0, MEM_RELEASE);
        } else {
            out("VirtualAlloc:           FAILED\r\n");
        }
    }

    /* Timing: Sleep really should advance GetTickCount. */
    {
        DWORD before = GetTickCount();
        Sleep(150);
        out_num("Sleep(150) advanced ms: ", GetTickCount() - before);
    }

    /* A file from the initrd, through the Win32 file API. */
    {
        HANDLE file = CreateFileA("hello.txt", GENERIC_READ, FILE_SHARE_READ, NULL,
                                  OPEN_EXISTING, 0, NULL);
        if (file != INVALID_HANDLE_VALUE) {
            char buffer[128];
            DWORD read = 0;
            out_num("hello.txt size:         ", GetFileSize(file, NULL));
            if (ReadFile(file, buffer, sizeof(buffer) - 1, &read, NULL)) {
                buffer[read] = '\0';
                out("hello.txt contents:     ");
                out(buffer);
                out("\r\n");
            }
            CloseHandle(file);
        } else {
            out_num("CreateFileA failed, err ", GetLastError());
        }
    }

    /* Console colour, which maps onto the framebuffer console's palette. */
    SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), FOREGROUND_GREEN |
                            FOREGROUND_INTENSITY);
    out("\r\nThis line should be bright green.\r\n");
    SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE),
                            FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);

    MessageBoxA(NULL, "Rendered on the console.", "MessageBoxA", MB_OK);

    out("\r\nCalling ExitProcess(7).\r\n");
    ExitProcess(7);
    return 0;
}
