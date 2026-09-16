/* chromeprobe64.c - why chrome_elf.dll refuses to initialise.
 *
 * Milestone 90 got chrome.exe loaded and running under Wine on this
 * kernel. It stops here:
 *
 *   err:module:loader_init "chrome_elf.dll" failed to initialize, aborting
 *   err:module:loader_init Initializing dlls for
 *       L"Z:\opt\chromium\chrome.exe" failed, status 80000003
 *
 * 0x80000003 is STATUS_BREAKPOINT - an `int3`, which in Chromium means
 * a CHECK failed and called IMMEDIATE_CRASH(). chrome_elf carries no
 * symbols, so the question is which check, and the honest way to answer
 * it is to ask the machine the same questions chrome_elf asks before
 * anything guesses.
 *
 * The three candidates, in the order chrome_elf's DllMain reaches them:
 *
 *   - the OS version. base/win/windows_version.cc is compiled into
 *     chrome_elf and Chromium 156 requires Windows 10 or later. Wine
 *     defaults a prefix to 10.0 build 19045, so this should pass - but
 *     "should" is what this program exists to replace.
 *
 *   - the product version. install_static reads chrome.exe's own
 *     VS_VERSION_INFO to work out the channel and install mode, and
 *     CHECKs what it finds. chrome.exe does carry the resource
 *     (ProductVersion 156.0.8062.0, confirmed on the host); whether
 *     Wine's version.dll can read it here is a different question.
 *
 *   - loading the DLL itself, which is the step that dies. Done last,
 *     and deliberately without a handler: everything above it has
 *     already been printed, so a program that ends here still reports.
 */

#include <windows.h>
#include <stdio.h>

static void say( const char *s )
{
    fputs( s, stdout );
    fputc( '\n', stdout );
    fflush( stdout );
}

/* Report the first breakpoint and where it came from, then let the
 * search continue so the loader still fails the way it would have. */
static LONG CALLBACK on_exception( EXCEPTION_POINTERS *ep )
{
    static int reported;
    DWORD code = ep->ExceptionRecord->ExceptionCode;

    if (!reported && (code == EXCEPTION_BREAKPOINT || code == 0x80000003))
    {
        void *addr = ep->ExceptionRecord->ExceptionAddress;
        MEMORY_BASIC_INFORMATION mbi;

        reported = 1;
        if (VirtualQuery( addr, &mbi, sizeof(mbi) ) && mbi.AllocationBase)
        {
            WCHAR name[MAX_PATH] = L"?";
            GetModuleFileNameW( (HMODULE)mbi.AllocationBase, name, MAX_PATH );
            printf( "     BREAKPOINT at %p = %ls + 0x%llx\n", addr, name,
                    (unsigned long long)((char *)addr - (char *)mbi.AllocationBase) );
        }
        else
            printf( "     BREAKPOINT at %p (no module)\n", addr );
        fflush( stdout );
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

#define CHROME_EXE L"Z:\\opt\\chromium\\chrome.exe"
#define CHROME_ELF L"Z:\\opt\\chromium\\chrome_elf.dll"

int main( void )
{
    OSVERSIONINFOEXW osv;
    DWORD handle = 0, size;
    HKEY key;
    HMODULE mod;

    say( "chromeprobe64: asking what chrome_elf asks" );

    /* --- where the breakpoint actually is --------------------------- *
     *
     * A vectored handler, installed before anything loads chrome_elf.
     * STATUS_BREAKPOINT names no place by itself, and chrome_elf ships
     * no symbols, but the exception record carries the address and
     * VirtualQuery turns that into a module base - so what comes out is
     * an RVA that can be disassembled on the host. That is the whole
     * difference between "a CHECK failed somewhere in chrome_elf" and a
     * line of code. */
    AddVectoredExceptionHandler( 1, on_exception );

    /* --- the OS version, as Chromium reads it ----------------------- *
     *
     * Two readings, because they disagree on purpose. GetVersionEx lies
     * to a program with no compatibility manifest - Windows has done
     * that since 8.1 and Wine copies it - and reports 6.2. Chromium does
     * not use it for exactly that reason: base/win/windows_version.cc
     * calls RtlGetVersion, which does not lie. So the number that
     * matters is the second one. */
    {
        LONG (WINAPI *rtl_get_version)( OSVERSIONINFOEXW * );
        HMODULE nt = GetModuleHandleW( L"ntdll.dll" );
        rtl_get_version = (void *)GetProcAddress( nt, "RtlGetVersion" );
        if (rtl_get_version)
        {
            OSVERSIONINFOEXW rv;
            memset( &rv, 0, sizeof(rv) );
            rv.dwOSVersionInfoSize = sizeof(rv);
            if (!rtl_get_version( &rv ))
                printf( "     RtlGetVersion = %lu.%lu build %lu   <- what Chromium reads\n",
                        rv.dwMajorVersion, rv.dwMinorVersion, rv.dwBuildNumber );
            else
                say( "     RtlGetVersion failed" );
        }
        else
            say( "     ntdll has no RtlGetVersion" );
        fflush( stdout );
    }


    memset( &osv, 0, sizeof(osv) );
    osv.dwOSVersionInfoSize = sizeof(osv);
    if (GetVersionExW( (OSVERSIONINFOW *)&osv ))
        printf( "     GetVersionEx  = %lu.%lu build %lu, platform %lu, sp %u.%u\n",
                osv.dwMajorVersion, osv.dwMinorVersion, osv.dwBuildNumber,
                osv.dwPlatformId, osv.wServicePackMajor, osv.wServicePackMinor );
    else
        printf( "     GetVersionEx failed, %lu\n", GetLastError() );
    fflush( stdout );

    /* The same answer from the registry, which is where chrome_elf's
     * strings say it also looks. */
    if (RegOpenKeyExW( HKEY_LOCAL_MACHINE,
                       L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                       0, KEY_READ, &key ) == ERROR_SUCCESS)
    {
        WCHAR buf[128];
        DWORD len = sizeof(buf), type = 0;
        if (RegQueryValueExW( key, L"CurrentVersion", NULL, &type,
                              (BYTE *)buf, &len ) == ERROR_SUCCESS)
            printf( "     registry CurrentVersion = %ls\n", buf );
        else
            say( "     registry CurrentVersion is not set" );
        len = sizeof(buf);
        if (RegQueryValueExW( key, L"CurrentBuildNumber", NULL, &type,
                              (BYTE *)buf, &len ) == ERROR_SUCCESS)
            printf( "     registry CurrentBuildNumber = %ls\n", buf );
        else
            say( "     registry CurrentBuildNumber is not set" );
        RegCloseKey( key );
    }
    else
        say( "     HKLM\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion missing" );
    fflush( stdout );

    /* --- chrome.exe's version resource ------------------------------ */

    size = GetFileVersionInfoSizeW( CHROME_EXE, &handle );
    printf( "     GetFileVersionInfoSize(chrome.exe) = %lu (err %lu)\n",
            size, size ? 0UL : GetLastError() );
    fflush( stdout );
    if (size)
    {
        void *data = malloc( size );
        if (data && GetFileVersionInfoW( CHROME_EXE, handle, size, data ))
        {
            VS_FIXEDFILEINFO *ffi = NULL;
            UINT ffi_len = 0;
            if (VerQueryValueW( data, L"\\", (void **)&ffi, &ffi_len ) && ffi)
                printf( "     chrome.exe file version = %u.%u.%u.%u\n",
                        HIWORD(ffi->dwFileVersionMS), LOWORD(ffi->dwFileVersionMS),
                        HIWORD(ffi->dwFileVersionLS), LOWORD(ffi->dwFileVersionLS) );
            else
                printf( "     VerQueryValue failed, %lu\n", GetLastError() );
        }
        else
            printf( "     GetFileVersionInfo failed, %lu\n", GetLastError() );
        free( data );
    }
    fflush( stdout );

    /* --- the DLL, as data and then for real ------------------------- */

    mod = LoadLibraryExW( CHROME_ELF, NULL, LOAD_LIBRARY_AS_DATAFILE );
    printf( "     LoadLibraryEx(AS_DATAFILE) = %p (err %lu)\n",
            (void *)mod, mod ? 0UL : GetLastError() );
    fflush( stdout );
    if (mod) FreeLibrary( mod );

    /* Last, and without a handler. If chrome_elf's DllMain breakpoints
     * the way it does under chrome.exe, this line is where this program
     * ends - and everything worth knowing is already above it. */
    say( "     loading chrome_elf for real..." );
    mod = LoadLibraryW( CHROME_ELF );
    printf( "     LoadLibrary(chrome_elf) = %p (err %lu)\n",
            (void *)mod, mod ? 0UL : GetLastError() );
    fflush( stdout );

    say( "chromeprobe64: survived" );
    return 0;
}
