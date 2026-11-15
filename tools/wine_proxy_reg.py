#!/usr/bin/env python3
"""wine_proxy_reg.py <host:port> - a .reg file that gives a Wine prefix an
HTTP proxy the way Windows keeps one (Milestone 113).

Two places, because two families of callers read two places: wininet's
ProxyEnable/ProxyServer under Internet Settings, and the binary
DefaultConnectionSettings under Internet Settings\\Connections, which is
all WinHttpGetIEProxyConfigForCurrentUser reads - and what Steam's
updater asks. The blob is a header (magic 0x46, a counter, flags:
direct | proxy), then three length-prefixed ASCII strings: the proxy,
the bypass list, the autoconfig URL."""
import struct, sys

proxy = sys.argv[1].encode("ascii")
bypass = b"<local>"
blob = struct.pack("<III", 0x46, 0, 0x3)
blob += struct.pack("<I", len(proxy)) + proxy
blob += struct.pack("<I", len(bypass)) + bypass
blob += struct.pack("<I", 0) + b"\0" * 32
hexs = ",".join("%02x" % b for b in blob)
print("Windows Registry Editor Version 5.00\r")
print("\r")
print("[HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings]\r")
print('"ProxyEnable"=dword:00000001\r')
print('"ProxyServer"="%s"\r' % sys.argv[1])
print('"ProxyOverride"="<local>"\r')
print("\r")
print("[HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings\\Connections]\r")
print('"DefaultConnectionSettings"=hex:%s\r' % hexs)
print('"WinHttpSettings"=hex:%s\r' % hexs)
