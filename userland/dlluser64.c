/* dlluser64.c - a 64-bit Windows program that ships its own DLL.
 *
 * The shape chrome.exe has: a small executable whose real work is in a
 * DLL beside it. chrome.exe imports from chrome_elf.dll, which is
 * Chrome's own and ships in the install; this imports from dlllib64.dll
 * the same way.
 */

typedef unsigned int   UINT;
typedef unsigned long  DWORD;
typedef void          *HANDLE;
typedef int            BOOL;

#define STD_OUTPUT_HANDLE ((DWORD)-11)

__declspec(dllimport) int         DllSayHello(void);
__declspec(dllimport) const char *DllGetMessage(void);

__declspec(dllimport) HANDLE GetStdHandle(DWORD nStdHandle);
__declspec(dllimport) BOOL   WriteFile(HANDLE hFile, const void *lpBuffer,
                                       DWORD nNumberOfBytesToWrite,
                                       DWORD *lpNumberOfBytesWritten,
                                       void *lpOverlapped);
__declspec(dllimport) void   ExitProcess(UINT uExitCode);

void entry(void)
{
    const char *m;
    DWORD written = 0, len = 0;
    int r = DllSayHello();

    /* DllGetMessage returns a pointer the DLL holds *in memory*, so it
     * is only correct if the loader fixed up that pointer when it moved
     * the DLL off its preferred base. Printing through it is what makes
     * the relocation load-bearing rather than merely counted: get it
     * wrong and this reads from the address the linker chose, which is
     * not mapped. */
    m = DllGetMessage();
    while (m[len]) len++;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), m, len, &written, 0);

    /* Two failures reported differently: 5 means the DLL ran and
     * returned what it should, 99 means the call arrived but came back
     * wrong - which points at the calling convention rather than at the
     * loader. */
    ExitProcess(r == 1234 ? 5 : 99);
}
