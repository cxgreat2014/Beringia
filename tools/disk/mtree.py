#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Read the system-volume mtree that ships with a macOS OTA and compare a staged root tree against it.
#
#   tools/disk/mtree.py list  MTREE                 one line per entry: type, inode, path
#   tools/disk/mtree.py check MTREE ROOT [--json OUT] [--fix-hardlinks]
#
# MTREE is the plain-text mtree (the "mtree.txt" member of the OTA's *.dmg.aea.x86.mtree; see tools/disk/README.md).
# It records every path of Apple's sealed system volume with its type and APFS inode number, so it is the reference
# for what an extracted tree must contain:
#   - paths missing from ROOT, by type (file, dir, link);
#   - hard links: Apple's archives store a hard-linked file once, so every other name of the same inode is lost unless
#     the extractor recreates it. --fix-hardlinks creates those names in ROOT with link(2), from any name of the inode
#     that is present (ROOT must be on a file system that supports hard links, e.g. ext4).
# Run as a user that can read all of ROOT (root, for a tree extracted with its original owners).
import argparse
import collections
import json
import os
import re
import sys

_ESC = re.compile(r"\\([0-7]{3}|.)")


def unvis(name):
    """mtree names are vis(3)-encoded: \\ooo octal bytes and a few backslash escapes."""
    raw = bytearray()
    pos = 0
    for m in _ESC.finditer(name):
        raw += name[pos:m.start()].encode("utf-8")
        e = m.group(1)
        if len(e) == 3:
            raw.append(int(e, 8))
        else:
            raw += {"s": b" ", "t": b"\t", "n": b"\n", "\\": b"\\"}.get(e, e.encode("utf-8"))
        pos = m.end()
    raw += name[pos:].encode("utf-8")
    return raw.decode("utf-8", "surrogateescape")


def logical_lines(f):
    buf = ""
    for line in f:
        line = line.rstrip("\n")
        if line.endswith("\\"):
            buf += line[:-1] + " "
            continue
        yield buf + line
        buf = ""
    if buf:
        yield buf


def parse(path):
    """Yield (relpath, keywords) for every entry; relpath is "" for the root."""
    defaults = {}
    stack = []
    with open(path, encoding="utf-8", errors="surrogateescape") as f:
        for line in logical_lines(f):
            s = line.strip()
            if not s or s.startswith("#"):
                continue
            words = s.split()
            if words[0] == "/set":
                for w in words[1:]:
                    k, _, v = w.partition("=")
                    defaults[k] = v
                continue
            if words[0] == "/unset":
                for k in words[1:]:
                    defaults.pop(k, None)
                continue
            if words[0] == "..":
                if not stack:
                    raise ValueError("'..' above the root")
                stack.pop()
                continue
            kw = dict(defaults)
            for w in words[1:]:
                k, _, v = w.partition("=")
                kw[k] = v
            name = unvis(words[0])
            if name == "." and not stack:
                rel = ""
            else:
                if "/" in name or name in (".", ".."):
                    raise ValueError("bad name %r" % name)
                rel = "/".join(stack[1:] + [name]) if stack else name
            yield rel, kw
            if kw.get("type") == "dir":
                stack.append(name)


def kind(st_mode):
    import stat
    if stat.S_ISDIR(st_mode):
        return "dir"
    if stat.S_ISLNK(st_mode):
        return "link"
    if stat.S_ISREG(st_mode):
        return "file"
    return "other"


def cmd_list(a):
    for rel, kw in parse(a.mtree):
        print(kw.get("type", "?"), kw.get("inode", "-"), rel or ".")


def cmd_check(a):
    root = a.root.rstrip("/")
    entries = list(parse(a.mtree))
    by_inode = collections.defaultdict(list)
    for rel, kw in entries:
        if kw.get("type") == "file" and kw.get("inode"):
            by_inode[kw["inode"]].append(rel)
    groups = {i: p for i, p in by_inode.items() if len(p) > 1}
    missing = collections.defaultdict(list)
    wrong = []
    present = 0
    for rel, kw in entries:
        if not rel:
            continue
        t = kw.get("type", "file")
        try:
            st = os.lstat(os.path.join(root, rel))
        except FileNotFoundError:
            missing[t].append(rel)
            continue
        except PermissionError:
            missing["unreadable"].append(rel)
            continue
        present += 1
        if kind(st.st_mode) != t:
            wrong.append((rel, t, kind(st.st_mode)))
    in_group = {p for ps in groups.values() for p in ps}
    hl_missing = [p for p in missing["file"] if p in in_group]
    fixed, unfixable = [], []
    if a.fix_hardlinks:
        miss = set(hl_missing)
        for ps in groups.values():
            have = [p for p in ps if p not in miss]
            for p in ps:
                if p not in miss:
                    continue
                if not have:
                    unfixable.append(p)
                    continue
                dst = os.path.join(root, p)
                if not os.path.isdir(os.path.dirname(dst)):
                    unfixable.append(p)
                    continue
                try:
                    os.link(os.path.join(root, have[0]), dst)
                except OSError as e:            # e.g. the name already exists: record it, do not stop the run
                    print("cannot link %s from %s: %s" % (p, have[0], e.strerror), file=sys.stderr)
                    unfixable.append(p)
                    continue
                fixed.append((p, have[0]))
    print("mtree entries: %d (%d hard-link groups, %d names)" % (len(entries), len(groups),
                                                               sum(len(p) for p in groups.values())))
    print("present: %d; type mismatches: %d" % (present, len(wrong)))
    for t in sorted(missing):
        print("missing %-10s %d" % (t, len(missing[t])))
    print("missing names of hard-linked files: %d" % len(hl_missing))
    if a.fix_hardlinks:
        print("hard links created: %d; not fixable (no name of the inode present, or no parent dir): %d" %
              (len(fixed), len(unfixable)))
    top = collections.Counter("/".join(p.split("/")[:3]) for t in missing for p in missing[t])
    print("missing, by top directories:")
    for d, n in top.most_common(25):
        print("  %7d  %s" % (n, d))
    if a.json:
        with open(a.json, "w") as f:
            json.dump({"missing": missing, "wrong_type": wrong, "hardlink_missing": hl_missing,
                       "hardlinks_created": fixed, "hardlinks_unfixable": unfixable}, f, indent=1)
    return 0


def main():
    p = argparse.ArgumentParser(description=__doc__)
    sub = p.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("list")
    s.add_argument("mtree")
    s.set_defaults(func=cmd_list)
    s = sub.add_parser("check")
    s.add_argument("mtree")
    s.add_argument("root")
    s.add_argument("--json")
    s.add_argument("--fix-hardlinks", action="store_true")
    s.set_defaults(func=cmd_check)
    a = p.parse_args()
    sys.exit(a.func(a) or 0)


if __name__ == "__main__":
    main()
