#!/bin/bash
# stage_wine.sh <wine-build-tree> <destination>
#
# Lays a Wine installation out the way Wine expects to find one, so that
# the loader can locate its own libraries from /proc/self/exe rather
# than being told where they are:
#
#   <dest>/usr/bin/wine                        the loader
#   <dest>/usr/bin/*.so                        the unix halves
#   <dest>/usr/bin/x86_64-windows/*.dll       the PE halves
#   <dest>/bin/wineserver                      the server
#
# The unix halves go *beside the loader*, not in an x86_64-unix
# directory, because that is where this Wine looks: it reads
# /proc/self/exe, takes the directory, and appends the library name. An
# installed Wine would find them under lib/wine/x86_64-unix; a Wine
# running from its build tree, which is what this is, does not. Getting
# that wrong produces exactly one line of output - "could not load
# ntdll.so: /usr/bin/ntdll.so: cannot open shared object file" - and it
# names the path it wanted, which is how this was settled.
#
# Only the modules in tools/wine_prefix_modules.txt are copied - the 93
# that creating a prefix was measured to load, out of the 602 the tree
# builds - and everything is stripped, which takes the PE half from
# 174MB to 58MB.
set -u

TREE="${1:-}"
DEST="${2:-}"
LIST="$(dirname "$0")/wine_prefix_modules.txt"

if [ -z "$TREE" ] || [ -z "$DEST" ]; then
    echo "usage: stage_wine.sh <wine-build-tree> <destination>" >&2
    exit 2
fi
if [ ! -x "$TREE/loader/wine" ]; then
    echo "stage_wine: no loader at $TREE/loader/wine" >&2
    exit 1
fi

BIN="$DEST/usr/bin"
UNIX="$DEST/usr/bin"
# Beside the loader, in an x86_64-windows directory next to it - the
# same rule as the unix halves and for the same reason (Milestone 71).
# A Wine running from its build tree takes the directory of its own
# /proc/self/exe and looks in <that>/x86_64-windows/ for a PE module;
# /usr/lib/wine is where an *installed* Wine keeps them and is not
# somewhere this Wine ever looks.
#
# Getting it wrong does not fail at load time. Wine finds wineboot.exe
# by its DOS path in the prefix, starts loading it, cannot resolve an
# import, and exits with 0xc0000135 - STATUS_DLL_NOT_FOUND - which names
# nothing.
WIN="$DEST/usr/bin/x86_64-windows"
mkdir -p "$BIN" "$UNIX" "$WIN" || exit 1

cp "$TREE/loader/wine" "$BIN/wine" || exit 1

# The wineserver, at the path wineboot actually spawns: it takes the
# directory of its own /proc/self/exe - /usr/bin - and asks for
# "../../bin/wineserver", which is /bin/wineserver. Not a guess; that is
# the string execve was handed, and without the file it answered -ENOENT
# and wineboot exited 127.
#
# Optional in the same way the loader is, because a tree may not have
# built it, and a missing server should say so here rather than as a
# spawn that fails much later.
SRV="$DEST/bin"
mkdir -p "$SRV" || exit 1
if [ -x "$TREE/server/wineserver" ]; then
    cp "$TREE/server/wineserver" "$SRV/wineserver" || exit 1
else
    echo "stage_wine: no wineserver at $TREE/server/wineserver" >&2
fi

# The unix halves. All of them: there are only 25 and the dependency
# graph between them is not something to guess at.
#
# In both places, for the same reason the NLS tables below are: the
# loader and ntdll work the path out differently and both are right
# about their own layout.
#
# The loader finds *ntdll.so* by reading /proc/self/exe, taking the
# directory and appending the name - /usr/bin/ntdll.so, which is what
# Milestone 71 established. ntdll then loads every other unix half
# itself, and it builds that path as dll_dir + get_so_dir(machine),
# which is /usr/bin/x86_64-unix/. Only the first of those two was
# staged, so ntdll.so loaded and nothing else did.
#
# It does not fail at load time either. The PE half of a module loads,
# its DllMain calls into a unix half that is not there, and the process
# exits 0xc0000142 - STATUS_DLL_INIT_FAILED - naming nothing. What the
# trace shows is one openat of
# /usr/bin/x86_64-unix/ws2_32.so returning -ENOENT and then a dead
# process.
#
# Stripped, the 25 of them are 4.9MB, so both copies cost less than the
# NLS tables did.
UNIX_ARCH="$DEST/usr/bin/x86_64-unix"
mkdir -p "$UNIX_ARCH" || exit 1
found_unix=0
while IFS= read -r so; do
    cp "$so" "$UNIX/$(basename "$so")" 2>/dev/null && found_unix=$((found_unix+1))
    cp "$so" "$UNIX_ARCH/$(basename "$so")" 2>/dev/null
done < <(find "$TREE/dlls" -name "*.so" -type f)

# Every shared library the unix halves ask for, checked here rather than
# discovered four layers downstream.
#
# Milestone 81 lost a long time to libm.so.6 being absent: win32u.so
# needs it, dlopen fails, win32u's unix half never loads, its init()
# never runs, KeAddSystemServiceTable is never called for table 1, and
# every win32u syscall comes back STATUS_INVALID_SYSTEM_SERVICE. user32
# does not check that, so 0xc000001c travels on as a GDI handle and the
# process dies in gdi32 on a null PEB->GdiSharedHandleTable. Nothing in
# that chain mentions libm.
#
# So the dependencies are read off the staged files with objdump and
# reported. This does not copy them - the initrd's library staging is
# the Makefile's job - it says out loud which ones a loader is going to
# go looking for, so that a missing one is a line here instead of a
# fault somewhere else.
if command -v objdump >/dev/null 2>&1; then
    need=$(for f in "$UNIX_ARCH"/*.so; do
               objdump -p "$f" 2>/dev/null | awk '/NEEDED/{print $2}'
           done | sort -u | grep -v '\.so$' | grep -vE '^(ntdll|win32u)\.so')
    echo "stage_wine: unix halves need:" $(echo $need | tr '\n' ' ')
fi

# The NLS tables. 77 files and 8KB in total, and not optional: the
# wineserver loads l_intl.nls before it will serve anything and dies
# with "failed to load l_intl.nls" without it - after binding its
# socket, so its atexit handler unlinks the socket on the way out and
# the client that follows reports "a wine server seems to be running,
# but I cannot connect to it". Two misleading messages from one missing
# 8KB directory.
# Twice, because the server and the client work the path out
# differently and both are right about their own layout. The server
# looks in /usr/share/wine/nls. The client takes the directory of its
# own /proc/self/exe - /usr/bin - and appends "../../share/wine/nls",
# which is /share/wine/nls: that arithmetic expects the loader two
# levels below the prefix, and this installation puts it in bin beside
# its unix libraries because that is where the loader looks for *them*
# (Milestone 71). Rather than move the loader and break that, the 8KB
# is staged in both places.
#
# Getting this wrong is not a missing translation. uctable stays NULL
# and the first ntdll_towupper reads through it - a null dereference in
# a case conversion, a long way from anything about locales.
# wine.inf, which is what actually builds the prefix.
#
# Not staged at all until Milestone 81, and its absence is quiet in the
# way this layer keeps being quiet: wineboot creates the directories,
# reports "created the configuration directory", and then
# update_wineprefix cannot open the file, takes its `goto done`, and
# never spawns the rundll32 that runs the inf. The prefix is left
# without system.reg, user.reg or userdef.reg - which is exactly the
# three of seven paths the layer was reporting missing - and wineboot
# exits 1.
#
# What it says, once stderr is read rather than the exit code:
#
#   wine: failed to update L"\??\Z:\root\.wine"
#         with L"\\?\Z:\share\wine\wine.inf":
#
# and that names the path it wanted. get_wine_inf_path builds it from
# the loader's own directory as ../../share/wine/wine.inf, which from
# /usr/bin is /share/wine - the same arithmetic the NLS tables need, so
# it is staged in both places for the same reason.
if [ -f "$TREE/loader/wine.inf" ]; then
    for d in "$DEST/usr/share/wine" "$DEST/share/wine"; do
        mkdir -p "$d" || exit 1
        cp "$TREE/loader/wine.inf" "$d/" || exit 1
    done
    echo "stage_wine: wine.inf, in both places"
else
    echo "stage_wine: no wine.inf at $TREE/loader/wine.inf" >&2
fi

# --- the display driver -------------------------------------------------
#
# Novaris has no X server and no Wayland compositor, so explorer.exe
# walks HKCU\Software\Wine\Drivers\Graphics, finds nothing it can load,
# and every program with a window gets
# "err:winediag:nodrv_CreateWindow ... The graphics driver is missing".
# winenovaris.drv is how Wine reaches this OS's own window manager
# (wine/winenovaris.drv, kernel/wmdev.c).
#
# What it looks like without this, several minutes into a prefix run, is
# explorer.exe starting and win32u reporting
# `err:win:get_desktop_window failed to create desktop window` - which
# names neither the driver nor the registry key.
#
# Skipped rather than fatal when it has not been built: an OS with Wine
# and no driver still runs console programs, and that is what every
# milestone before the driver was.
DRV_PE="$TREE/dlls/winenovaris.drv/x86_64-windows/winenovaris.drv"
DRV_UNIX="$TREE/dlls/winenovaris.drv/winenovaris.so"
if [ -f "$DRV_PE" ] && [ -f "$DRV_UNIX" ]; then
    cp "$DRV_PE" "$WIN/winenovaris.drv" || exit 1
    # Both places, for the same reason every other unix half is in both:
    # which one Wine looks in depends on whether it thinks it is
    # installed or running from its build tree.
    for d in "$UNIX" "$UNIX_ARCH"; do
        mkdir -p "$d" || exit 1
        cp "$DRV_UNIX" "$d/winenovaris.so" || exit 1
    done

    # And tell Wine to use it. explorer.exe's built-in default list is
    # "mac,x11,wayland", none of which exists here; this key is the
    # supported way to change it, and a prefix gets it when wineboot
    # runs wine.inf. Patched into the staged copies rather than the Wine
    # tree, so the tree stays a build input.
    #
    # Only the first AddReg list is touched - that is [BaseInstall]'s,
    # which every install in the file needs.
    for d in "$DEST/usr/share/wine" "$DEST/share/wine"; do
        [ -f "$d/wine.inf" ] || continue
        if ! grep -q "NovarisDrivers" "$d/wine.inf"; then
            sed -i -e '0,/^AddReg=\\$/s||AddReg=\\\n    NovarisDrivers,\\|' \
                "$d/wine.inf" || exit 1
            printf '\n[NovarisDrivers]\nHKCU,Software\\Wine\\Drivers,"Graphics",2,"novaris"\n' \
                >> "$d/wine.inf" || exit 1
        fi
    done
    echo "stage_wine: winenovaris.drv, both halves, and the registry key"
else
    echo "stage_wine: no winenovaris.drv - build it with tools/build_wine_driver.sh"
fi

if [ -d "$TREE/nls" ]; then
    for d in "$DEST/usr/share/wine/nls" "$DEST/share/wine/nls"; do
        mkdir -p "$d" || exit 1
        cp "$TREE"/nls/*.nls "$d/" 2>/dev/null
    done
    echo "stage_wine: $(ls "$DEST/usr/share/wine/nls" | wc -l) NLS tables, in both places"
else
    echo "stage_wine: no nls directory at $TREE/nls" >&2
fi

# The PE halves, from the measured list.
found_pe=0
missing=""
while IFS= read -r name; do
    case "$name" in ''|\#*) continue ;; esac
    f=$(find "$TREE" -name "$name" -path "*x86_64-windows*" -type f 2>/dev/null | head -1)
    if [ -n "$f" ]; then
        cp "$f" "$WIN/$name" && found_pe=$((found_pe+1))
    else
        missing="$missing $name"
    fi
done < "$LIST"

# Stripping is not tidiness here - it is what makes the initrd fit.
find "$DEST/usr" "$DEST/bin" -type f -exec strip --strip-debug {} \; 2>/dev/null

echo "stage_wine: $found_pe PE modules, $found_unix unix libraries, $(du -sh "$DEST/usr" | cut -f1)"
echo "stage_wine: wineserver $([ -x "$SRV/wineserver" ] && echo staged || echo MISSING)"
[ -n "$missing" ] && echo "stage_wine: NOT FOUND:$missing" >&2
exit 0
