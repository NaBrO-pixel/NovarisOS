#!/usr/bin/env python3
"""wine_font_reg.py > fonts.reg - stand-ins for the Windows core fonts
(Milestone 113).

Web pages name their fonts the Windows way - "Arial, Helvetica,
sans-serif" - and Chromium maps the generic families to Arial, Times New
Roman and Courier New as well. A Wine prefix has none of these. Chromium
126 (the one inside Steam's client) then draws nothing at all: a family
that is not there gives no font and no per-character fallback either, so
Steam's sign-in window came up with every label missing whenever its own
web font (Motiva Sans) was not ready.

Wine's DirectWrite system collection takes aliases from
HKCU\\Software\\Wine\\Fonts\\Replacements (as GDI does): each name that is
missing becomes an alias of its replacement, if that one exists. The
Liberation fonts are metric-compatible with the core fonts and are staged
with Wine (Makefile.amd64); without them (--tahoma) Wine's own Tahoma, always installed, stands in.

The values are plain strings, one replacement each: `reg import` widens
the bytes of a hex(7) multi-string a second time, so a list written that
way arrives as single letters and replaces nothing.
"""
import sys

LIBERATION = {
    "Arial": "Liberation Sans",
    "Helvetica": "Liberation Sans",
    "Segoe UI": "Liberation Sans",
    "Verdana": "Liberation Sans",
    "Microsoft Sans Serif": "Liberation Sans",
    "Times New Roman": "Liberation Serif",
    "Times": "Liberation Serif",
    "Georgia": "Liberation Serif",
    "Courier New": "Liberation Mono",
    "Consolas": "Liberation Mono",
}
WINE_ONLY = {"Liberation Sans": "Tahoma", "Liberation Serif": "Tahoma", "Liberation Mono": "Tahoma"}


def main():
    tahoma = "--tahoma" in sys.argv[1:]
    out = ["Windows Registry Editor Version 5.00", "",
           "[HKEY_CURRENT_USER\\Software\\Wine\\Fonts\\Replacements]"]
    for name, repl in LIBERATION.items():
        out.append('"%s"="%s"' % (name, WINE_ONLY[repl] if tahoma else repl))
    sys.stdout.write("\r\n".join(out) + "\r\n")


main()
