#!/bin/sh
# Stages Mesa's EGL and its software renderer into an initrd tree.
#
#   tools/stage_mesa.sh <stage-dir>
#
# What OpenGL and Vulkan need with no window system and no GPU
# (Milestone 110). For OpenGL:
# glvnd's libEGL, which a program links against; the vendor file under
# /usr/share/glvnd/egl_vendor.d that tells it Mesa is the implementation;
# Mesa's libEGL_mesa, which it then dlopens; and libgallium, where
# llvmpipe lives, with LLVM behind it. For Vulkan: the loader, lavapipe
# (libvulkan_lvp), and its manifest under /usr/share/vulkan/icd.d. Every library is closed under ldd
# and staged under the soname the loader asks for, as stage_wine.sh does
# for Wine's own dlopens - a dlopen that fails on a dependency says
# nothing more useful than one that fails on the library.
#
# Roughly 250MB, most of it libLLVM and ICU, which is why it is opt-in
# (MESA64_INSTALL=1) rather than part of every Wine image.
set -e

DEST=$1
LIB="$DEST/lib/x86_64-linux-gnu"
if [ -z "$DEST" ]; then
    echo "usage: $0 <stage-dir>" >&2
    exit 1
fi

seeds="libEGL.so.1 libEGL_mesa.so.0 libGLdispatch.so.0 libvulkan.so.1 libvulkan_lvp.so"
mkdir -p "$LIB" "$DEST/usr/share/glvnd/egl_vendor.d" "$DEST/usr/share/vulkan/icd.d"

staged=0
for so in $seeds; do
    path=$(ldconfig -p | awk -v s="$so" '$1==s {print $NF; exit}')
    [ -z "$path" ] && [ -f "/usr/lib/x86_64-linux-gnu/$so" ] && path="/usr/lib/x86_64-linux-gnu/$so"
    if [ -z "$path" ]; then
        echo "stage_mesa: $so is not on this host - install libegl-mesa0 and mesa-vulkan-drivers" >&2
        exit 1
    fi
    for pair in "$so|$path" $(ldd "$path" |
                    sed -n 's/^[[:space:]]*\([^ ]*\) => \([^ ]*\).*/\1|\2/p'); do
        name=${pair%%|*}
        file=${pair#*|}
        case "$name" in
            # The Makefile stages these, the exact build ld.so is given.
            libc.so.6|libm.so.6|libgcc_s.so.1|ld-linux*) continue ;;
        esac
        [ -f "$LIB/$name" ] && continue
        cp "$file" "$LIB/$name"
        staged=$((staged+1))
    done
done

for f in /usr/share/glvnd/egl_vendor.d/*mesa*.json; do
    cp "$f" "$DEST/usr/share/glvnd/egl_vendor.d/"
done
# Lavapipe alone: the loader tries every manifest it finds, and the GPU
# drivers' would each probe for hardware that is not there.
cp /usr/share/vulkan/icd.d/lvp_icd*.json "$DEST/usr/share/vulkan/icd.d/"

echo "stage_mesa: $staged libraries, $(du -sm "$LIB" | cut -f1)MB in lib/x86_64-linux-gnu"
