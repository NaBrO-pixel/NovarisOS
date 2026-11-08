#!/usr/bin/env python3
"""screenshots.py MONITOR_SOCKET OUT_DIR INTERVAL MARKER_FILE MARKER

Takes a QEMU `screendump` through the monitor socket every INTERVAL
seconds once MARKER has appeared in MARKER_FILE (the serial log), and
stops when QEMU goes away. Each one is saved as OUT_DIR/shot-NN.png, so
a run with a window on the screen leaves a record of what it showed and
when. Used by make chrome-window.
"""

import os
import socket
import sys
import time

from PIL import Image


def monitor(sock_path, command):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(sock_path)
    with s:
        s.settimeout(5.0)
        try:
            s.recv(4096)
        except socket.timeout:
            pass
        s.sendall(command.encode() + b"\n")
        time.sleep(1.0)
        try:
            return s.recv(65536).decode(errors="replace")
        except socket.timeout:
            return ""


def main():
    sock, out, interval, marker_file, marker = sys.argv[1:6]
    interval = float(interval)
    os.makedirs(out, exist_ok=True)
    while True:
        try:
            with open(marker_file, "rb") as f:
                if marker.encode() in f.read():
                    break
        except OSError:
            pass
        if not os.path.exists(sock):
            time.sleep(1)
            continue
        time.sleep(2)
    n = 0
    while True:
        ppm = os.path.join(out, "shot.ppm")
        try:
            monitor(sock, "screendump %s" % ppm)
        except OSError:
            return 0                                  # QEMU has gone
        if os.path.exists(ppm):
            Image.open(ppm).save(os.path.join(out, "shot-%02d.png" % n))
            os.remove(ppm)
            print("screenshots: shot-%02d.png at %s" % (n, time.strftime("%T")),
                  flush=True)
            n += 1
        time.sleep(interval)


if __name__ == "__main__":
    sys.exit(main())
