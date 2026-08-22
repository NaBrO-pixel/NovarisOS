#!/bin/bash
# stage_wine.sh <wine-build-tree> <destination>
#
# Lays a Wine installation out the way Wine expects to find one, so that
# the loader can locate its own libraries from /proc/self/exe rather
# than being told where they are:
#
#   <dest>/usr/bin/wine                        the loader
#   <dest>/usr/bin/*.so                        the unix halves
#   <dest>/usr/lib/wine/x86_64-windows/*.dll   the PE halves
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
WIN="$DEST/usr/lib/wine/x86_64-windows"
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
found_unix=0
while IFS= read -r so; do
    cp "$so" "$UNIX/$(basename "$so")" 2>/dev/null && found_unix=$((found_unix+1))
done < <(find "$TREE/dlls" -name "*.so" -type f)

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
