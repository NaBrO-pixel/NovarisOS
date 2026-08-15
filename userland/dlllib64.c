/* dlllib64.c - a real 64-bit Windows DLL.
 *
 * It exports a function, and it *imports* one from kernel32 itself, so
 * loading it exercises the transitive case: the executable's import has
 * to resolve to an address inside this module, and this module's own
 * import has to resolve to a thunk, before either can run.
 *
 * -nostdlib, so DllEntry stands in for DllMainCRTStartup. Nothing calls
 * it yet - see the note on DllMain in ROADMAP.md - and it exists only
 * because a DLL must have an entry point for the linker to record.
 */

typedef unsigned long  DWORD;
typedef void          *HANDLE;
typedef int            BOOL;

#define STD_OUTPUT_HANDLE ((DWORD)-11)

__declspec(dllimport) HANDLE GetStdHandle(DWORD nStdHandle);
__declspec(dllimport) BOOL   WriteFile(HANDLE hFile, const void *lpBuffer,
                                       DWORD nNumberOfBytesToWrite,
                                       DWORD *lpNumberOfBytesWritten,
                                       void *lpOverlapped);

static const char msg[] = "hello from a DLL, called by a 64-bit PE\n";

/* Exported data, initialised to an address. This is what forces an
 * IMAGE_REL_BASED_DIR64 relocation: the value has to exist in memory as
 * a full 64-bit pointer, so the linker cannot fold it into a
 * RIP-relative computation the way it does with a static const. Without
 * something like it this DLL has no .reloc section at all, and the
 * loader's relocation path would go untested - which matters, because
 * chrome.exe most certainly does have one. */
__declspec(dllexport) const char *DllMessagePtr = msg;

__declspec(dllexport) const char *DllGetMessage(void)
{
    return DllMessagePtr;   /* a load from memory, not a lea */
}

__declspec(dllexport) int DllSayHello(void)
{
    DWORD written = 0;

    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), msg, sizeof(msg) - 1,
              &written, 0);

    /* A value the caller checks, so that "the DLL ran" and "the DLL
     * returned correctly" are two different assertions. */
    return 1234;
}

/* Takes its arguments in the Windows convention like everything else
 * here; returning non-zero is what DLL_PROCESS_ATTACH expects. */
__declspec(dllexport) int DllEntry(HANDLE inst, DWORD reason, void *reserved)
{
    (void)inst; (void)reason; (void)reserved;
    return 1;
}
