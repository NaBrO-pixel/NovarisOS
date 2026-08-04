#!/bin/sh
# Installs Wine into a Novaris root directory, at the paths an installed
# Wine actually lives at.
#
#   tools/install_wine.sh <wine-build-tree> <novaris-root> [strip]
#
# Why the paths matter, and why this is a script rather than four `cp`
# lines in the Makefile.
#
# Wine works out where it is from where ntdll.so was loaded, and then
# derives everything else from that by *relative* arithmetic
# (dlls/ntdll/unix/loader.c, init_paths and build_relative_path). With
# ntdll.so at <prefix>/lib/wine/i386-unix/ntdll.so it concludes:
#
#     dll_dir   = <prefix>/lib/wine          the builtins, both halves
#     bin_dir   = <prefix>/bin               wineserver lives here
#     data_dir  = <prefix>/share/wine        wine.inf and the NLS tables
#     wineloader= <dll_dir>/i386-unix/wine   what it re-execs
#
# Through Milestone 34 Novaris shipped Wine as a heap of files at the root
# of a flat initrd, and every one of those four came out wrong. It worked
# anyway because the kernel has a fallback that matches the last component
# of a path whose directories do not exist - so "/lib32/libc.so.6" found
# "/libc.so.6" and nobody had to notice. What it could not paper over was
# data_dir: a path that does not resolve at all has no NT form, so
# WINEDATADIR was never set, so wineboot could not find wine.inf, so the
# prefix could never be updated. "wine: failed to update ..., wine.inf not
# found" was the whole of Wine not being installed, said once.
#
# So: install it. The layout below is what `make install` produces, and
# Wine's own arithmetic lands on it without being told anything.
set -e

WINE_BUILD=$1
ROOT=$2
DO_STRIP=${3:-1}

if [ -z "$WINE_BUILD" ] || [ -z "$ROOT" ]; then
    echo "usage: $0 <wine-build-tree> <novaris-root> [strip]" >&2
    exit 1
fi
if [ ! -f "$WINE_BUILD/loader/wine-preloader" ]; then
    echo "$WINE_BUILD does not look like a built Wine tree" >&2
    exit 1
fi

HOST_LIB32=${HOST_LIB32:-/lib32}

# The subset that ships. Kept as a list rather than a wildcard so that
# what is installed is a decision rather than whatever happened to be
# built - the initrd is read into RAM whole, and Wine builds 601 DLLs.
PE_DLLS="ntdll apisetschema kernel32 kernelbase win32u user32 gdi32 advapi32
         sechost rpcrt4 msvcrt ucrtbase ws2_32 setupapi version
         imm32 combase ole32 oleaut32 shell32 shlwapi shcore winex11
         wow64cpu cryptbase bcrypt userenv coml2 wininet mpr
         comctl32 comdlg32 winspool.drv"
# rundll32 and the setupapi machinery behind it are what wineboot needs to
# *update* a prefix from wine.inf - which is how a prefix comes to exist
# at all. Milestone 35 is the first time they have had wine.inf to read.
PE_PROGS="wineboot start conhost services explorer rundll32 cmd winepath
          reg regsvr32 notepad winemine"
# A Wine builtin is two halves: a PE .dll the Windows program links
# against, and a Unix .so it reaches through __wine_init_unix_call().
# Shipping only the first half is not "that feature is missing" - it is a
# DLL whose process attach fails.
UNIX_DLLS="win32u ws2_32 bcrypt"

# Novaris's own display driver, built into the same tree by
# tools/build_wine_driver.sh. Not in the lists above because it is not
# one of Wine's - and because both its halves live in one directory
# rather than in the two the loops below expect.
DRV_PE=$WINE_BUILD/dlls/winenovaris.drv/i386-windows/winenovaris.drv
DRV_UNIX=$WINE_BUILD/dlls/winenovaris.drv/winenovaris.so

UNIXDIR=$ROOT/usr/lib/wine/i386-unix
PEDIR=$ROOT/usr/lib/wine/i386-windows
BINDIR=$ROOT/usr/bin
DATADIR=$ROOT/usr/share/wine

mkdir -p "$UNIXDIR" "$PEDIR" "$BINDIR" "$DATADIR/nls" \
         "$ROOT/lib" "$ROOT/lib32" "$ROOT/etc"

# --- Wine's Unix half ---------------------------------------------------
#
# ntdll.so is the one Wine finds itself by; the loader and the preloader
# sit beside it because that is where init_paths() looks for them
# ("<ntdll dir>/wine", and the preloader is that name plus "-preloader").
cp "$WINE_BUILD/dlls/ntdll/ntdll.so"        "$UNIXDIR/ntdll.so"
cp "$WINE_BUILD/loader/wine"                "$UNIXDIR/wine"
cp "$WINE_BUILD/loader/wine-preloader"      "$UNIXDIR/wine-preloader"
for d in $UNIX_DLLS; do
    cp "$WINE_BUILD/dlls/$d/$d.so" "$UNIXDIR/$d.so" 2>/dev/null || true
done

# The wrapper a user runs, and the server it starts.
#
# /usr/bin/wine is a *different binary* from the loader in i386-unix, and
# the difference is the whole reason the bin directory works: the loader
# finds ntdll.so beside itself, which is only true where it is installed,
# while the wrapper (tools/wine/wine.c) works out bindir from its own
# path, turns that into libdir with the same relative arithmetic used
# above, and dlopens <libdir>/wine/i386-unix/ntdll.so. Installing the
# loader here instead gets "could not load ntdll.so: /usr/bin/ntdll.so",
# which is what the first attempt at this did.
#
# From there Wine takes over: ntdll re-execs the real loader through the
# preloader on its own, so "wine prog.exe" is the whole invocation.
cp "$WINE_BUILD/tools/wine/wine"   "$BINDIR/wine"
cp "$WINE_BUILD/server/wineserver" "$BINDIR/wineserver"

# --- Wine's Windows half ------------------------------------------------
for d in $PE_DLLS; do
    cp "$WINE_BUILD/dlls/$d/i386-windows/$d.dll" "$PEDIR/$d.dll" 2>/dev/null || true
done
for p in $PE_PROGS; do
    cp "$WINE_BUILD/programs/$p/i386-windows/$p.exe" "$PEDIR/$p.exe" 2>/dev/null || true
done

# --- the display driver -------------------------------------------------
#
# Without one, every program with a window gets
# "err:winediag:nodrv_CreateWindow ... The graphics driver is missing" and
# exits: explorer.exe reaches load_graphics_driver(), tries each name in
# HKCU\Software\Wine\Drivers\Graphics, and finds nothing to load. Novaris
# has no X server and no Wayland compositor - it has its own window
# manager, and winenovaris.drv is how Wine reaches it (wine/winenovaris.drv,
# kernel/wmdev.c).
#
# Skipped rather than fatal when it has not been built: an OS with Wine
# and no driver is exactly what every milestone up to 35 was, and it still
# runs console programs.
if [ -f "$DRV_PE" ] && [ -f "$DRV_UNIX" ]; then
    cp "$DRV_PE"   "$PEDIR/winenovaris.drv"
    cp "$DRV_UNIX" "$UNIXDIR/winenovaris.so"

    # And a Windows program with a window, where a user will find it: the
    # root of the filesystem is what the File Explorer opens on, and
    # double-clicking a .exe there runs it under Wine (kernel/app_files.c).
    # Until there was a display driver there was no reason to put a GUI
    # program in front of anybody.
    cp "$PEDIR/notepad.exe" "$ROOT/notepad.exe" 2>/dev/null || true
    cp "$PEDIR/winemine.exe" "$ROOT/winemine.exe" 2>/dev/null || true

    # And tell Wine to use it. explorer.exe's default list is
    # "mac,x11,wayland", none of which exists here; this key is the
    # supported way to change it, and a prefix gets it when wineboot
    # installs wine.inf. Added to the copy rather than to the Wine tree,
    # so the tree stays a build input.
    # Only the first AddReg list, which is [BaseInstall]'s: every install
    # in the file needs it, and every install needs [BaseInstall].
    sed -e '0,/^AddReg=\\$/s||AddReg=\\\n    NovarisDrivers,\\|' \
        "$WINE_BUILD/loader/wine.inf" > "$DATADIR/wine.inf.tmp"
    printf '\n[NovarisDrivers]\nHKCU,Software\\Wine\\Drivers,"Graphics",2,"novaris"\n' \
        >> "$DATADIR/wine.inf.tmp"
    WINE_INF=$DATADIR/wine.inf.tmp
else
    echo "  (no winenovaris.drv - build it with tools/build_wine_driver.sh)"
fi

# --- Wine's data --------------------------------------------------------
#
# wine.inf is the script rundll32 runs to populate a prefix: the registry
# keys, the fake DLLs, the directory layout. Without it wineboot has
# nothing to install and says so.
cp "${WINE_INF:-$WINE_BUILD/loader/wine.inf}" "$DATADIR/wine.inf"
rm -f "$DATADIR/wine.inf.tmp"
# The NLS tables, whole rather than hand-picked: wineserver calls
# fatal_error() if it cannot load l_intl.nls, and kernelbase's
# init_locale walks sortdefault.nls without checking that it got it. A
# missing table does not announce itself.
cp "$WINE_BUILD"/nls/*.nls "$DATADIR/nls/"

# --- the host's C library ----------------------------------------------
#
# /lib32 because that is the first directory ld-linux.so.2 looks in for
# an i386 program on this host, and /lib because that is the interpreter
# path in every one of these binaries' PT_INTERP.
cp "$HOST_LIB32/ld-linux.so.2" "$ROOT/lib/ld-linux.so.2"
cp "$HOST_LIB32/ld-linux.so.2" "$ROOT/lib32/ld-linux.so.2"
cp "$HOST_LIB32/libc.so.6"     "$ROOT/lib32/libc.so.6"
# glibc unwinds a thread out of itself with _Unwind_ForcedUnwind, which
# lives in libgcc_s.so.1 and is dlopen()ed the first time pthread_exit()
# is called - so a library nothing links against is a hard requirement
# for a *thread* to end.
cp "$HOST_LIB32/libgcc_s.so.1" "$ROOT/lib32/libgcc_s.so.1"

# Everything else anything here links against, worked out rather than
# listed. The hand-written list was wrong for two milestones: win32u.so
# needs libm.so.6 and nothing shipped it, so the Unix half of win32u
# could not be dlopen'd - and win32u is the Unix backend for both gdi32
# and user32. Ask the binaries instead of remembering.
for f in "$UNIXDIR"/*.so "$UNIXDIR/wine" "$BINDIR"/wine "$BINDIR"/wineserver; do
    [ -f "$f" ] || continue
    objdump -p "$f" 2>/dev/null | awk '/NEEDED/ { print $2 }'
done | sort -u | while read -r lib; do
    [ -f "$ROOT/lib32/$lib" ] && continue
    [ -f "$HOST_LIB32/$lib" ] || continue
    echo "  + $lib (needed by a Wine binary)"
    cp "$HOST_LIB32/$lib" "$ROOT/lib32/$lib"
done

# --- the two files glibc's getpwuid() needs -----------------------------
#
# Wine dereferences its result without checking. Real paths now, so
# /etc/passwd is a file at /etc/passwd.
printf 'root:x:0:0:root:/root:/bin/sh\n' > "$ROOT/etc/passwd"
printf 'passwd: files\ngroup: files\n'   > "$ROOT/etc/nsswitch.conf"

# --- strip ---------------------------------------------------------------
#
# Debug information is three quarters of the bytes and nothing here reads
# it. It is the difference between an initrd that fits in RAM and one
# that does not.
if [ "$DO_STRIP" != "0" ]; then
    i686-w64-mingw32-strip "$PEDIR"/*.dll "$PEDIR"/*.exe 2>/dev/null || true
    strip "$UNIXDIR"/*.so "$UNIXDIR/wine" "$BINDIR/wine" \
          "$BINDIR/wineserver" 2>/dev/null || true
fi

echo "Installed Wine into $ROOT:"
echo "  $(ls "$UNIXDIR" | wc -l) files in /usr/lib/wine/i386-unix"
echo "  $(ls "$PEDIR" | wc -l) files in /usr/lib/wine/i386-windows"
echo "  $(du -sh "$ROOT/usr" | cut -f1) total under /usr"
