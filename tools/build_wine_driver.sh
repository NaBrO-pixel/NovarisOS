#!/bin/sh
# Builds winenovaris.drv - Novaris's Wine display driver - inside a Wine
# source tree.
#
#   tools/build_wine_driver.sh <wine-source-tree>
#
# Why it is done this way.
#
# A Wine display driver is two halves that have to be built by two
# different toolchains: a PE module (the thing explorer.exe loads) and a
# Unix .so that links against libwin32u. Wine's build system already knows
# how to produce that pair from one directory of sources - winebuild, the
# spec file, the -mno-cygwin split, the import libraries - and
# reimplementing it outside the tree would be reimplementing winegcc.
#
# So the *source* lives here, in wine/winenovaris.drv/, under version
# control with the rest of Novaris, and this script grafts it into a Wine
# tree to be built. Wine stays a build input rather than something
# vendored, and the driver stays ours.
#
# What it does to the tree: copies the sources into dlls/winenovaris.drv/,
# adds one WINE_CONFIG_MAKEFILE line to configure.ac, re-runs autoconf and
# config.status, and makes the two halves. All of that is idempotent -
# running it twice is a no-op followed by an incremental build.
set -e

WINE_SRC=$1
HERE=$(cd "$(dirname "$0")/.." && pwd)
SRC="$HERE/wine/winenovaris.drv"

if [ -z "$WINE_SRC" ]; then
    echo "usage: $0 <wine-source-tree>" >&2
    exit 1
fi
if [ ! -f "$WINE_SRC/configure.ac" ]; then
    echo "$WINE_SRC is not a Wine source tree" >&2
    exit 1
fi
if [ ! -f "$WINE_SRC/config.status" ]; then
    echo "$WINE_SRC has not been configured - run its configure first" >&2
    exit 1
fi

DST="$WINE_SRC/dlls/winenovaris.drv"
mkdir -p "$DST"
cp "$SRC"/Makefile.in "$SRC"/*.c "$SRC"/*.h "$DST/"

# One line in configure.ac, next to the other drivers, so that configure
# generates a Makefile for the new directory. Adding it by hand rather
# than through tools/make_makefiles because that script rewrites the whole
# list and would drag in unrelated churn.
if ! grep -q 'WINE_CONFIG_MAKEFILE(dlls/winenovaris.drv)' "$WINE_SRC/configure.ac"; then
    echo "adding dlls/winenovaris.drv to configure.ac"
    sed -i 's|^WINE_CONFIG_MAKEFILE(dlls/winemapi)$|WINE_CONFIG_MAKEFILE(dlls/winenovaris.drv)\n&|' \
        "$WINE_SRC/configure.ac"
    grep -q 'WINE_CONFIG_MAKEFILE(dlls/winenovaris.drv)' "$WINE_SRC/configure.ac" || {
        echo "could not find a place to add it in configure.ac" >&2
        exit 1
    }
    NEED_CONFIGURE=1
fi

# And one line in explorer.
#
# explorer.exe decides which driver to load: it walks a comma-separated
# list of names, loads the first `wine<name>.drv` it finds, and records it
# in the registry for win32u to pick up. The list comes from
# HKCU\Software\Wine\Drivers\Graphics, and falls back to a built-in
# default of "mac,x11,wayland" - three drivers, none of which this OS has
# or could have.
#
# tools/install_wine.sh sets that registry value, which is the supported
# way to do this and the one a user could change. This is the other half:
# a prefix that already exists - built by an earlier Wine, or living on a
# disk from before the driver was written - has no such value, and the
# default is what it gets. Putting "novaris" at the front of the default
# means a driver that is installed is a driver that is used, registry or
# no registry.
#
# One `sed`, and it is idempotent.
if ! grep -q 'novaris,mac,x11,wayland' "$WINE_SRC/programs/explorer/desktop.c"; then
    echo "adding novaris to explorer's default driver list"
    sed -i 's|L"mac,x11,wayland"|L"novaris,mac,x11,wayland"|' \
        "$WINE_SRC/programs/explorer/desktop.c"
    grep -q 'novaris,mac,x11,wayland' "$WINE_SRC/programs/explorer/desktop.c" || {
        echo "could not patch explorer's default driver list" >&2
        exit 1
    }
fi

# The generated Makefile is what `make` reads; it comes from configure,
# which comes from configure.ac.
if [ -n "$NEED_CONFIGURE" ] || [ ! -f "$WINE_SRC/dlls/winenovaris.drv/Makefile" ]; then
    echo "regenerating Wine's build files (this takes a minute)"
    (cd "$WINE_SRC" && autoconf -o configure configure.ac)
    (cd "$WINE_SRC" && ./config.status --recheck >/dev/null)
    (cd "$WINE_SRC" && ./config.status >/dev/null)
fi

# Both halves by name rather than the directory: `make dlls/<dir>` builds
# whatever that directory's default target is, which for a driver is the
# PE module alone.
echo "building winenovaris.drv"
(cd "$WINE_SRC" && make -j"$(nproc)" \
    dlls/winenovaris.drv/i386-windows/winenovaris.drv \
    dlls/winenovaris.drv/winenovaris.so \
    programs/explorer/i386-windows/explorer.exe)

# What tools/install_wine.sh looks for.
for f in "$WINE_SRC/dlls/winenovaris.drv/i386-windows/winenovaris.drv" \
         "$WINE_SRC/dlls/winenovaris.drv/winenovaris.so"; do
    [ -f "$f" ] || { echo "missing after build: $f" >&2; exit 1; }
    echo "  $f"
done
echo "winenovaris.drv built."
