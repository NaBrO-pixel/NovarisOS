#!/bin/sh
# qemu_net64.sh RESOLV_CONF COMMAND... - run COMMAND (QEMU) seeing
# RESOLV_CONF as /etc/resolv.conf, in a mount namespace of its own.
#
# Why: the guest's resolver asks QEMU's user-mode DNS forwarder at
# 10.0.2.3 - the server DHCP hands it - and the forwarder asks whatever
# this machine's /etc/resolv.conf names. Pointing that at 127.0.0.1,
# where tools/net64_helper.py --dns53 answers, makes a name lookup inside
# NovarisOS travel the whole real path - glibc or Wine's resolver, UDP
# out through the card, QEMU, an answer back - without the guest being
# configured any differently from a normal run, and without touching
# this machine's own resolver.
#
# Needs root for unshare and mount. Without them the command runs as it
# is, the guest's lookups go to this machine's real resolver, and make
# test says the name checks could not be made.
conf="$1"; shift
if [ "$(id -u)" = 0 ] && unshare -m true 2>/dev/null; then
    exec unshare -m sh -c 'mount --bind "$0" /etc/resolv.conf && exec "$@"' "$conf" "$@"
fi
exec "$@"
