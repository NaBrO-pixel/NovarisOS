#!/bin/sh
# update_e2e.sh - the updater, end to end, on one machine.
#
# Milestone 40's `update` is the one feature that cannot be tested by
# reading its output, because what it claims to have done and what it
# actually did are the same sentence. The only honest test is to install a
# version onto a disk, reboot off that disk, and ask the machine what
# version it is.
#
# So this builds twice. The running kernel is whatever the tree says; the
# *served* kernel is the tree with NOVARIS_VERSION bumped by one, built
# into a scratch directory and published with a manifest. The guest boots
# the ISO, fetches the manifest, installs onto an attached FAT32 disk, and
# is then booted a second time. The disc stays in the drive, because the
# disc is the bootloader - what the second boot tests is GRUB's
# `search --file /boot/version` finding the disk's copy and booting that
# instead of its own. If the machine comes back up as the new version the
# updater works. If it comes back as the old one it does not, however
# cheerful its first-boot output was.
#
#     tools/tests/update_e2e.sh
#
# Takes a few minutes: two kernel builds and two boots.

set -e

cd "$(dirname "$0")/../.."
ROOT=$(pwd)
WORK=${WORK:-$(mktemp -d)}
SERVE="$WORK/serve"

VERSION_FILE=kernel/update.c
CUR=$(sed -n 's/^#define NOVARIS_VERSION *\([0-9]*\).*/\1/p' "$VERSION_FILE")
if [ -z "$CUR" ]; then
    echo "could not read NOVARIS_VERSION from $VERSION_FILE" >&2
    exit 1
fi
NEXT=$((CUR + 1))

echo "== current version $CUR, publishing $NEXT"

mkdir -p "$SERVE"

# The newer build. Done first and put out of the way, so that whatever
# happens afterwards the tree is left holding the version it started with.
restore() {
    sed -i "s/^#define NOVARIS_VERSION .*/#define NOVARIS_VERSION $CUR/" \
        "$ROOT/$VERSION_FILE"
    sed -i "s/^#define NOVARIS_NAME .*/#define NOVARIS_NAME    \"$CUR_NAME\"/" \
        "$ROOT/$VERSION_FILE"
}
CUR_NAME=$(sed -n 's/^#define NOVARIS_NAME *"\(.*\)".*/\1/p' "$VERSION_FILE")
trap restore EXIT INT TERM

sed -i "s/^#define NOVARIS_VERSION .*/#define NOVARIS_VERSION $NEXT/" "$VERSION_FILE"
sed -i "s/^#define NOVARIS_NAME .*/#define NOVARIS_NAME    \"Milestone $NEXT\"/" "$VERSION_FILE"
make -s >/dev/null
cp build/novaris.bin build/initrd.img "$SERVE/"

# Back to the version under test, and rebuilt, so the ISO that boots is
# genuinely the older one. Testing an updater with a disc that already
# holds the new version proves nothing.
restore
make -s >/dev/null

# The manifest names the URLs the guest will fetch, so the port has to be
# decided here rather than by the harness: a manifest is a file, and a
# file cannot be told what port it is being served on.
PORT=${PORT:-38080}
python3 tools/make_manifest.py "$SERVE" "http://10.0.2.2:$PORT" "$NEXT" \
        "Novaris $NEXT" > "$SERVE/latest.manifest"

# A blank FAT32 disk for the updater to install onto. 64MB is room for a
# kernel and an initrd twice over.
DISK="$WORK/disk.img"
python3 tools/mkfat32.py "$DISK" --size 64M --label NOVARIS

echo "== boot 1: install $NEXT onto the disk"
python3 tools/qemu_test.py --iso novaris.iso --disk "$DISK" \
    --memory 768 --boot-wait 40 --settle 240 --timeout 400 \
    --http-dir "$SERVE" --http-port "$PORT" \
    --serial-log "$WORK/boot1.log" \
    --setup "net up" \
    --cmd "update apply http://10.0.2.2:$PORT/latest.manifest" \
    --post-cmd "sync" \
    --expect "Update installed to /disk/boot" --stop-when-matched

# The disk now has a version marker and a kernel beside it, so the disc's
# GRUB must boot those. A guest that comes up saying the old version means
# the updater wrote something GRUB did not find - a failure the first
# boot's own output cannot show.
echo "== boot 2: the disc's GRUB has to pick the disk's kernel"
python3 tools/qemu_test.py --iso novaris.iso --disk "$DISK" \
    --memory 768 --boot-wait 40 --settle 8 --timeout 120 \
    --serial-log "$WORK/boot2.log" \
    --cmd "update" \
    --expect "This is Novaris Milestone $NEXT \(version $NEXT\)"

echo "== OK: the machine came back up as version $NEXT"
echo "logs in $WORK"
