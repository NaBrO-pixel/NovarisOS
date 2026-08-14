#!/usr/bin/env python3
"""
pe_imports.py - what a Windows binary actually asks the OS for.

Reads a PE (PE32 or PE32+), lists every DLL and function it imports -
both the ordinary import table and the delay-load table, which is where
Chrome keeps user32, gdi32 and the graphics stack - and, given a Wine
tree, reports which of those Wine can actually supply.

The coverage answer comes from Wine's own .spec files. A .spec is the
authoritative list of what a Wine DLL exports, so "is this function
implemented" is a question the tree can answer without building it. A
spec line that is marked stub is counted as a stub, not as an export:
the symbol resolves and the call returns failure, which is the
difference between a program that starts and a program that works.

Usage:
    pe_imports.py BINARY                       # what it needs
    pe_imports.py BINARY --wine ../wine        # what Wine has
    pe_imports.py BINARY --wine ../wine --missing-only
    pe_imports.py BINARY --json
"""

import argparse
import json
import os
import re
import struct
import sys

# ---------------------------------------------------------------- PE parsing


class PEError(Exception):
    pass


class PE:
    """Enough of a PE reader to answer import questions on 32- and 64-bit."""

    def __init__(self, path):
        with open(path, "rb") as fh:
            self.data = fh.read()
        self.path = path
        self._parse_headers()

    # -- primitives

    def u8(self, off):
        return self.data[off]

    def u16(self, off):
        return struct.unpack_from("<H", self.data, off)[0]

    def u32(self, off):
        return struct.unpack_from("<I", self.data, off)[0]

    def u64(self, off):
        return struct.unpack_from("<Q", self.data, off)[0]

    def _parse_headers(self):
        d = self.data
        if len(d) < 0x40 or d[:2] != b"MZ":
            raise PEError("not a DOS/PE image (no MZ)")
        pe_off = self.u32(0x3C)
        if pe_off + 24 > len(d) or d[pe_off:pe_off + 4] != b"PE\0\0":
            raise PEError("no PE signature")
        self.pe_off = pe_off

        coff = pe_off + 4
        self.machine = self.u16(coff)
        self.n_sections = self.u16(coff + 2)
        size_opt = self.u16(coff + 16)
        self.characteristics = self.u16(coff + 18)

        opt = coff + 20
        self.opt = opt
        magic = self.u16(opt)
        if magic == 0x10B:
            self.pe32_plus = False
        elif magic == 0x20B:
            self.pe32_plus = True
        else:
            raise PEError("unknown optional header magic 0x%x" % magic)

        if self.pe32_plus:
            self.image_base = self.u64(opt + 24)
            self.subsystem = self.u16(opt + 68)
            self.dll_characteristics = self.u16(opt + 70)
            n_dirs_off = opt + 108
            dirs_off = opt + 112
        else:
            self.image_base = self.u32(opt + 28)
            self.subsystem = self.u16(opt + 68)
            self.dll_characteristics = self.u16(opt + 70)
            n_dirs_off = opt + 92
            dirs_off = opt + 96

        self.n_dirs = self.u32(n_dirs_off)
        self.dirs = []
        for i in range(self.n_dirs):
            base = dirs_off + i * 8
            if base + 8 > len(d):
                break
            self.dirs.append((self.u32(base), self.u32(base + 4)))

        sec_off = opt + size_opt
        self.sections = []
        for i in range(self.n_sections):
            b = sec_off + i * 40
            if b + 40 > len(d):
                break
            name = d[b:b + 8].rstrip(b"\0").decode("latin-1")
            self.sections.append({
                "name": name,
                "vsize": self.u32(b + 8),
                "vaddr": self.u32(b + 12),
                "rawsize": self.u32(b + 16),
                "rawptr": self.u32(b + 20),
            })

    # -- address translation

    def rva_to_off(self, rva):
        """File offset for an RVA, or None if it lands outside the image."""
        for s in self.sections:
            # A section's mapped span is the larger of its raw and virtual
            # size; tools disagree on which is authoritative, and using the
            # max keeps tables that straddle the boundary readable.
            span = max(s["vsize"], s["rawsize"])
            if s["vaddr"] <= rva < s["vaddr"] + span:
                delta = rva - s["vaddr"]
                if delta >= s["rawsize"]:
                    return None  # in the zero-fill tail, nothing on disk
                return s["rawptr"] + delta
        return None

    def cstr(self, off, limit=512):
        if off is None or off >= len(self.data):
            return None
        end = self.data.find(b"\0", off, off + limit)
        if end < 0:
            end = off + limit
        return self.data[off:end].decode("latin-1")

    # -- imports

    def _thunks(self, rva):
        """Walk a thunk array, yielding ('name', str) or ('ordinal', int)."""
        out = []
        step = 8 if self.pe32_plus else 4
        ord_flag = (1 << 63) if self.pe32_plus else (1 << 31)
        off = self.rva_to_off(rva)
        if off is None:
            return out
        while True:
            if off + step > len(self.data):
                break
            val = self.u64(off) if self.pe32_plus else self.u32(off)
            if val == 0:
                break
            if val & ord_flag:
                out.append(("ordinal", val & 0xFFFF))
            else:
                name_off = self.rva_to_off(val & 0x7FFFFFFF)
                nm = self.cstr(name_off + 2) if name_off is not None else None
                out.append(("name", nm) if nm else ("ordinal", -1))
            off += step
        return out

    def imports(self):
        """{dll: [names...]} from the ordinary import directory."""
        result = {}
        if len(self.dirs) < 2:
            return result
        rva, _size = self.dirs[1]
        if not rva:
            return result
        off = self.rva_to_off(rva)
        if off is None:
            return result
        while off + 20 <= len(self.data):
            orig_thunk = self.u32(off)
            name_rva = self.u32(off + 12)
            first_thunk = self.u32(off + 16)
            if orig_thunk == 0 and name_rva == 0 and first_thunk == 0:
                break
            dll = self.cstr(self.rva_to_off(name_rva)) or "<unknown>"
            funcs = self._thunks(orig_thunk or first_thunk)
            result.setdefault(dll, [])
            result[dll].extend(self._fmt(funcs))
            off += 20
        return result

    def delay_imports(self):
        """{dll: [names...]} from the delay-load directory."""
        result = {}
        if len(self.dirs) < 14:
            return result
        rva, _size = self.dirs[13]
        if not rva:
            return result
        off = self.rva_to_off(rva)
        if off is None:
            return result
        while off + 32 <= len(self.data):
            attrs = self.u32(off)
            name_rva = self.u32(off + 4)
            int_rva = self.u32(off + 16)
            iat_rva = self.u32(off + 12)
            if name_rva == 0 and int_rva == 0:
                break
            # Bit 0 clear means the fields are virtual addresses, not RVAs -
            # the pre-VC7 layout. Rebase them so the same reader handles both.
            if not (attrs & 1):
                if name_rva > self.image_base:
                    name_rva -= self.image_base
                if int_rva > self.image_base:
                    int_rva -= self.image_base
                if iat_rva > self.image_base:
                    iat_rva -= self.image_base
            dll = self.cstr(self.rva_to_off(name_rva)) or "<unknown>"
            funcs = self._thunks(int_rva or iat_rva)
            result.setdefault(dll, [])
            result[dll].extend(self._fmt(funcs))
            off += 32
        return result

    @staticmethod
    def _fmt(funcs):
        out = []
        for kind, val in funcs:
            out.append(val if kind == "name" else "#%d" % val)
        return out

    # -- description

    def arch(self):
        return {0x014C: "i386", 0x8664: "x86_64",
                0xAA64: "arm64", 0x01C4: "arm"}.get(self.machine,
                                                    "0x%x" % self.machine)

    def kind(self):
        return "DLL" if self.characteristics & 0x2000 else "EXE"

    def subsystem_name(self):
        return {1: "native", 2: "GUI", 3: "console"}.get(self.subsystem,
                                                         str(self.subsystem))


# -------------------------------------------------------------- Wine coverage

# A spec line is "ordinal type name" with optional flags; stub and -stub
# both mean the export exists but does nothing useful.
SPEC_LINE = re.compile(
    r"^\s*(?P<ord>\d+|@)\s+(?P<type>[a-z]+)\s+(?P<rest>.*)$")


def parse_spec(path):
    """Return (exports, stubs) as sets of names from one Wine .spec file."""
    exports, stubs = set(), set()
    try:
        with open(path, "r", errors="replace") as fh:
            lines = fh.readlines()
    except OSError:
        return exports, stubs

    for line in lines:
        line = line.split("#", 1)[0].rstrip()
        if not line.strip():
            continue
        m = SPEC_LINE.match(line)
        if not m:
            continue
        typ = m.group("type")
        rest = m.group("rest").strip()
        if typ == "stub":
            # "@ stub NameHere"
            name = rest.split("(")[0].split()[0] if rest else None
            if name:
                stubs.add(name.lstrip("-"))
            continue
        if typ not in ("stdcall", "cdecl", "varargs", "extern", "thiscall",
                       "fastcall", "syscall"):
            continue
        # Flags such as -private or -arch=win64 sit between type and name.
        toks = rest.split()
        name = None
        for t in toks:
            if t.startswith("-"):
                continue
            name = t.split("(")[0]
            break
        if not name:
            continue
        if " -stub" in (" " + rest) or rest.startswith("-stub"):
            stubs.add(name)
        else:
            exports.add(name)
    return exports, stubs


def wine_dll_index(wine_root):
    """Map lowercase dll name -> {'exports': set, 'stubs': set, 'spec': path}."""
    dlls_dir = os.path.join(wine_root, "dlls")
    index = {}
    if not os.path.isdir(dlls_dir):
        raise SystemExit("no dlls/ directory under %s" % wine_root)
    for entry in sorted(os.listdir(dlls_dir)):
        d = os.path.join(dlls_dir, entry)
        if not os.path.isdir(d):
            continue
        for fn in os.listdir(d):
            if not fn.endswith(".spec"):
                continue
            base = fn[:-5].lower()
            exports, stubs = parse_spec(os.path.join(d, fn))
            if not exports and not stubs:
                continue
            rec = index.setdefault(base, {"exports": set(), "stubs": set(),
                                          "spec": os.path.join(d, fn)})
            rec["exports"] |= exports
            rec["stubs"] |= stubs
    return index


def dll_key(name):
    n = name.lower()
    return n[:-4] if n.endswith(".dll") else n


def shipped_dlls(install_script):
    """The PE_DLLS set that tools/install_wine.sh actually installs.

    The list is assembled across several `PE_DLLS="$PE_DLLS ..."` lines, so
    this reads them all rather than the first. install_wine.sh is the only
    place the list exists (Milestone 43 deleted the copy in the Makefile
    precisely so it could not drift), which is why it is parsed here rather
    than duplicated a third time.
    """
    try:
        with open(install_script, "r", errors="replace") as fh:
            text = fh.read()
    except OSError as exc:
        raise SystemExit("cannot read %s: %s" % (install_script, exc))

    names = set()
    for m in re.finditer(r'PE_DLLS="([^"]*)"', text):
        body = m.group(1).replace("$PE_DLLS", " ")
        for tok in body.split():
            names.add(dll_key(tok))
    return names


def global_name_index(index):
    """name -> [dll, ...] across every spec, for resolving indirect imports.

    A binary rarely imports from the DLL that implements the function.
    `api-ms-win-core-synch-l1-2-0.dll` is an API set that Windows redirects
    to kernelbase; `WakeByAddressSingle` is a forwarder from kernelbase to
    ntdll; `RoInitialize` lives in combase whoever asks for it. Treating any
    of those as missing because the named DLL has no spec would overstate
    the gap badly - on chrome.exe it turns 2 real holes into 13.
    """
    out = {}
    for dll, rec in index.items():
        for fn in rec["exports"]:
            out.setdefault(fn, []).append(dll)
    return out


# --------------------------------------------------------------------- report


def collect(pe):
    """Merge normal and delay imports into one ordered mapping."""
    merged = {}
    for dll, funcs in pe.imports().items():
        merged.setdefault(dll, {"funcs": set(), "delay": False})
        merged[dll]["funcs"] |= set(funcs)
    for dll, funcs in pe.delay_imports().items():
        rec = merged.setdefault(dll, {"funcs": set(), "delay": True})
        rec["funcs"] |= set(funcs)
        if dll not in pe.imports():
            rec["delay"] = True
    return merged


def report_ship(merged, install_script, wine_index):
    """Which DLLs this binary needs that the OS image does not install.

    Split three ways, because the three mean different work: Wine has it and
    it is simply not in the ship list (add a name), Wine does not have it at
    all (write a DLL), or it is not Wine's to provide because the program
    ships it itself.
    """
    have = shipped_dlls(install_script)
    add, write, own = [], [], []
    for dll, rec in merged.items():
        key = dll_key(dll)
        if key in have:
            continue
        n = len(rec["funcs"])
        if key.startswith("api-ms-win-"):
            continue  # an API set, resolved through apisetschema
        if wine_index is not None and key in wine_index:
            add.append((dll, n))
        elif wine_index is not None:
            write.append((dll, n))
        else:
            own.append((dll, n))

    print("\n-- against tools/install_wine.sh --")
    if add:
        print("\n  in Wine, not shipped (%d) - add to PE_DLLS:" % len(add))
        for dll, n in sorted(add, key=lambda x: -x[1]):
            print("      %-28s %4d functions" % (dll, n))
    if write:
        print("\n  not in Wine at all (%d):" % len(write))
        for dll, n in sorted(write, key=lambda x: -x[1]):
            print("      %-28s %4d functions" % (dll, n))
    if own:
        print("\n  not shipped, Wine coverage unknown (%d):" % len(own))
        for dll, n in sorted(own, key=lambda x: -x[1]):
            print("      %-28s %4d functions" % (dll, n))
    if not (add or write or own):
        print("  every DLL this binary needs is already shipped.")


def main():
    ap = argparse.ArgumentParser(
        description="List what a PE imports, and what Wine can supply.")
    ap.add_argument("binary")
    ap.add_argument("--wine", metavar="DIR",
                    help="Wine source tree to check coverage against")
    ap.add_argument("--missing-only", action="store_true",
                    help="only print functions Wine has no export for")
    ap.add_argument("--functions", action="store_true",
                    help="print every function name, not just counts")
    ap.add_argument("--ship-list", metavar="INSTALL_SH",
                    help="tools/install_wine.sh, to report which needed "
                         "DLLs the OS image does not currently ship")
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()

    try:
        pe = PE(args.binary)
    except (PEError, OSError) as exc:
        sys.exit("%s: %s" % (args.binary, exc))

    merged = collect(pe)
    index = wine_dll_index(args.wine) if args.wine else None
    gnames = global_name_index(index) if index else {}

    report = {
        "binary": args.binary,
        "arch": pe.arch(),
        "kind": pe.kind(),
        "subsystem": pe.subsystem_name(),
        "image_base": pe.image_base,
        "dlls": [],
    }

    for dll in sorted(merged, key=lambda d: -len(merged[d]["funcs"])):
        funcs = sorted(merged[dll]["funcs"])
        rec = {
            "dll": dll,
            "delay": merged[dll]["delay"],
            "imported": len(funcs),
        }
        if index is not None:
            key = dll_key(dll)
            have = index.get(key)
            missing, stubbed, elsewhere = [], [], []
            for f in funcs:
                if f.startswith("#"):
                    continue  # ordinal import, not answerable from a spec
                if have is not None and f in have["exports"]:
                    continue
                other = gnames.get(f)
                if other:
                    elsewhere.append((f, other[0]))
                    continue
                if have is not None and f in have["stubs"]:
                    stubbed.append(f)
                else:
                    missing.append(f)
            rec["wine"] = "absent" if have is None else "present"
            rec["missing"] = missing
            rec["stubbed"] = stubbed
            rec["elsewhere"] = elsewhere
        if args.functions or args.json:
            rec["functions"] = funcs
        report["dlls"].append(rec)

    if args.json:
        print(json.dumps(report, indent=2))
        return

    print("%s: %s %s, subsystem %s, base 0x%x"
          % (os.path.basename(args.binary), pe.arch(), pe.kind(),
             pe.subsystem_name(), pe.image_base))
    total = sum(len(v["funcs"]) for v in merged.values())
    n_delay = sum(1 for v in merged.values() if v["delay"])
    print("%d DLLs (%d delay-loaded), %d imported functions\n"
          % (len(merged), n_delay, total))

    if index is None:
        for rec in report["dlls"]:
            tag = " (delay)" if rec["delay"] else ""
            print("  %-28s %4d%s" % (rec["dll"], rec["imported"], tag))
            if args.functions:
                for f in rec["functions"]:
                    print("      %s" % f)
        if args.ship_list:
            report_ship(merged, args.ship_list, None)
        return

    print("  %-34s %6s %6s %6s %5s" %
          ("DLL", "needs", "miss", "stub", "fwd"))
    tot_missing = tot_stub = tot_fwd = 0
    for rec in report["dlls"]:
        tot_missing += len(rec["missing"])
        tot_stub += len(rec["stubbed"])
        tot_fwd += len(rec["elsewhere"])
        print("  %-34s %6d %6d %6d %5d%s"
              % (rec["dll"], rec["imported"], len(rec["missing"]),
                 len(rec["stubbed"]), len(rec["elsewhere"]),
                 " (delay)" if rec["delay"] else ""))
        if args.functions or args.missing_only:
            for f in rec["missing"]:
                print("      MISSING %s" % f)
            if not args.missing_only:
                for f in rec["stubbed"]:
                    print("      stub    %s" % f)
                for f, where in rec["elsewhere"]:
                    print("      via %-14s %s" % (where, f))
    print("\n%d imported functions have no Wine export anywhere, "
          "%d resolve only to stubs,\n%d resolve through another Wine DLL "
          "(API set or forwarder)." % (tot_missing, tot_stub, tot_fwd))

    if args.ship_list:
        report_ship(merged, args.ship_list, index)


if __name__ == "__main__":
    main()
