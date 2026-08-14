/* hello_pe64.c - a real 64-bit Windows program.
 *
 * Built by x86_64-w64-mingw32-gcc into an ordinary PE32+ executable. It
 * imports from kernel32.dll, is linked at a Windows image base, and has
 * no idea Novaris exists - the same relationship userland/hello_glibc64.c
 * has with Linux.
 *
 * Deliberately -nostdlib with its own entry point. mingw's CRT startup
 * wants a great deal more of Win32 than exists here, and the thing being
 * tested is the loader and the calling convention, not the CRT.
 *
 * Note there is no __stdcall on x86-64: Windows has exactly one calling
 * convention there (arguments in rcx, rdx, r8, r9, then the stack, with
 * 32 bytes of shadow space the caller must reserve), and it is not the
 * SysV one the kernel is compiled for. That mismatch is what the import
 * thunks in pe64.c exist to bridge.
 */

typedef unsigned long  DWORD;
typedef unsigned int   UINT;
typedef void          *HANDLE;
typedef int            BOOL;

#define STD_OUTPUT_HANDLE ((DWORD)-11)

__declspec(dllimport) HANDLE GetStdHandle(DWORD nStdHandle);
__declspec(dllimport) BOOL   WriteFile(HANDLE hFile, const void *lpBuffer,
                                       DWORD nNumberOfBytesToWrite,
                                       DWORD *lpNumberOfBytesWritten,
                                       void *lpOverlapped);
__declspec(dllimport) void   ExitProcess(UINT uExitCode);

void entry(void)
{
    static const char msg[] =
        "hello from a 64-bit Windows PE on Novaris\n";
    HANDLE out;
    DWORD written = 0;

    out = GetStdHandle(STD_OUTPUT_HANDLE);
    WriteFile(out, msg, sizeof(msg) - 1, &written, 0);

    /* A distinctive status, and the only way out: there is no CRT to
     * return to. */
    ExitProcess(3);
}
