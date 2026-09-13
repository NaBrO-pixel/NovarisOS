#!/usr/bin/env python3
"""
close_wine_modules.py - close the staged module list under PE imports.

tools/wine_prefix_modules.txt was measured, by running wineboot with
WINEDEBUG=+loaddll and writing down what loaded. That is the right way
to find the seeds and the wrong way to finish the list, because a module
only gets measured if the run reached it - and a run does not reach a
module whose dependency is missing. Milestone 87 found three of those at
once:

    err:module:import_dll Library winspool.drv (which is needed by
        L"C:\\windows\\system32\\spool\\prtprocs\\x64\\winprint.dll")
        not found

winprint.dll was on the list and winspool.drv was not, so the measured
list contained a module that could never load. The same shape hid
rpcss.exe, which nothing loads because nothing can: it is started as a
service, and a service that is not on disk is one the SCM cannot open -
reported four layers away as `err:ole:start_rpcss Failed to open RpcSs
service` and then as COM marshalling failing with E_NOINTERFACE.

So: take the measured list as seeds, read what each one actually
imports - ordinary and delay-load, which is where the interesting ones
hide - and add anything the Wine tree can supply, to a fixpoint.

    close_wine_modules.py <wine-tree> [--write] [--seeds extra.txt]

Without --write it reports and changes nothing.
"""

import argparse
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
LIST = os.path.join(HERE, "wine_prefix_modules.txt")
PE_IMPORTS = os.path.join(HERE, "pe_imports.py")

# Imports that are not Wine modules and never will be: the PE side of
# this Wine calls into its own unix halves by these names, and asking the
# tree for a file called "ntdll.so" as a PE module finds nothing. Listed
# so that "not found in the tree" keeps meaning something.
NOT_MODULES = {"ntdll.so", "win32u.so"}


def read_list(path):
    """The list, and the comment header, kept apart so --write can put
    the header back unchanged."""
    header, names = [], []
    with open(path) as f:
        for line in f:
            s = line.strip()
            if not s or s.startswith("#"):
                if not names:
                    header.append(line.rstrip("\n"))
                continue
            names.append(s)
    return header, names


def find_in_tree(tree, name):
    """Where the tree keeps a PE module of this name, or None."""
    for root, dirs, files in os.walk(tree):
        if "x86_64-windows" not in root:
            continue
        if name in files:
            return os.path.join(root, name)
    return None


def index_tree(tree):
    """One walk instead of one per name: the tree is large and this is
    called for every module at every round."""
    index = {}
    for root, dirs, files in os.walk(tree):
        if "x86_64-windows" not in root:
            continue
        for f in files:
            index.setdefault(f.lower(), os.path.join(root, f))
    return index


def imports_of(path):
    """Every DLL this binary imports, delay-loaded ones included."""
    try:
        out = subprocess.run([sys.executable, PE_IMPORTS, path, "--json"],
                             capture_output=True, text=True, timeout=120)
        if out.returncode != 0:
            return None
        return [d["dll"] for d in json.loads(out.stdout).get("dlls", [])]
    except Exception:
        return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("tree", help="the Wine build tree")
    ap.add_argument("--write", action="store_true",
                    help="rewrite wine_prefix_modules.txt with the closure")
    ap.add_argument("--seed", action="append", default=[],
                    help="an extra module to start from, repeatable")
    args = ap.parse_args()

    header, seeds = read_list(LIST)
    index = index_tree(args.tree)
    print("tree holds %d PE files" % len(index))

    have = list(dict.fromkeys(seeds + args.seed))
    added, missing, unresolved = [], [], {}

    pending = list(have)
    seen_lower = {n.lower() for n in have}

    while pending:
        name = pending.pop(0)
        path = index.get(name.lower())
        if not path:
            missing.append(name)
            continue
        deps = imports_of(path)
        if deps is None:
            continue
        for d in deps:
            dl = d.lower()
            if dl in seen_lower or d in NOT_MODULES:
                continue
            if dl in index:
                seen_lower.add(dl)
                have.append(d)
                added.append((d, name))
                pending.append(d)
            else:
                unresolved.setdefault(d, set()).add(name)

    print("\nseeds: %d   after closure: %d   added: %d"
          % (len(seeds) + len(args.seed), len(have), len(added)))

    if added:
        print("\nadded, and what asked for it:")
        for d, by in sorted(added):
            print("    %-28s <- %s" % (d, by))

    if missing:
        print("\non the list but not in the tree (%d):" % len(missing))
        for m in sorted(missing):
            print("    %s" % m)

    if unresolved:
        print("\nimported but the tree has no such PE (%d) - these are"
              " imports Wine answers from a unix half or does not have:"
              % len(unresolved))
        for d in sorted(unresolved):
            print("    %-28s <- %s" % (d, ", ".join(sorted(unresolved[d]))))

    total = 0
    for n in have:
        p = index.get(n.lower())
        if p:
            total += os.path.getsize(p)
    print("\nunstripped total: %.1f MB (%d modules)"
          % (total / 1048576.0, len(have)))

    if args.write:
        with open(LIST, "w") as f:
            for line in header:
                f.write(line + "\n")
            for n in sorted(have, key=str.lower):
                f.write(n + "\n")
        print("\nwrote %s" % LIST)
    else:
        print("\n(dry run - pass --write to update the list)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
