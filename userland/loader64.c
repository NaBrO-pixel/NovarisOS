/* loader64.c - a Windows program that loads a DLL by name at run time.
 *
 * Everything before this milestone had its libraries resolved by the
 * kernel before it started: the import table named them, and the loader
 * bound them because the bytes were already compiled into the kernel.
 * This one names a file, at run time, and gets back a module - which is
 * what LoadLibrary actually is, and what chrome.exe's shape depends on
 * (chrome_elf.dll loads chrome.dll rather than importing it).
 *
 * Note it does NOT import DllSayHello. Its import table mentions only
 * kernel32; the DLL is found by string, and the function through
 * GetProcAddress.
 */

typedef unsigned int   UINT;
typedef unsigned long  DWORD;
typedef void          *HANDLE;
typedef void          *HMODULE;
typedef int            BOOL;

#define STD_OUTPUT_HANDLE ((DWORD)-11)

__declspec(dllimport) HMODULE GetStdHandle(DWORD nStdHandle);
__declspec(dllimport) BOOL    WriteFile(HANDLE hFile, const void *lpBuffer,
                                        DWORD nNumberOfBytesToWrite,
                                        DWORD *lpNumberOfBytesWritten,
                                        void *lpOverlapped);
__declspec(dllimport) HMODULE LoadLibraryA(const char *lpLibFileName);
__declspec(dllimport) void   *GetProcAddress(HMODULE hModule,
                                             const char *lpProcName);
__declspec(dllimport) void    ExitProcess(UINT uExitCode);

static void say(const char *s)
{
    DWORD written = 0, len = 0;
    while (s[len]) len++;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), s, len, &written, 0);
}

void entry(void)
{
    HMODULE mod;
    int (*say_hello)(void);
    HMODULE again;

    mod = LoadLibraryA("dlllib64.dll");
    if (!mod) {
        say("LoadLibraryA failed\n");
        ExitProcess(90);
    }
    say("LoadLibraryA returned a module\n");

    /* Asking twice must give the same handle back rather than a second
     * copy of the module - a DLL's data is per-process. */
    again = LoadLibraryA("dlllib64.dll");
    if (again != mod) {
        say("second LoadLibraryA returned a different handle\n");
        ExitProcess(91);
    }

    say_hello = (int (*)(void))GetProcAddress(mod, "DllSayHello");
    if (!say_hello) {
        say("GetProcAddress failed\n");
        ExitProcess(92);
    }

    /* Calling through a pointer the kernel resolved out of the DLL's own
     * export table, in a module this program named by string. */
    if (say_hello() != 1234) {
        say("the DLL returned the wrong value\n");
        ExitProcess(93);
    }

    /* A name that is not there has to fail, and fail without taking the
     * process with it. */
    if (LoadLibraryA("nosuchlibrary.dll") != 0) {
        say("loading a missing DLL should have failed\n");
        ExitProcess(94);
    }

    ExitProcess(11);
}
