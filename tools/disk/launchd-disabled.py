#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Disable (or re-enable) launchd jobs on a mounted macOS system volume, through launchd's own override database
# /private/var/db/com.apple.xpc.launchd/disabled.plist: the job's plist stays untouched, so --undo is exact.
#
#   tools/disk/launchd-disabled.py [--undo] LABEL... ROOT
#
# ROOT is the volume's mount point (last, so that scripts/run/experiment.sh --image-patch can append it). Prints the
# labels disabled afterwards. Keeps the file's format (binary or XML). Standard library only; run it with python3 -I.
import os
import plistlib
import sys


def main(argv):
    undo = False
    if argv and argv[0] == "--undo":
        undo, argv = True, argv[1:]
    if len(argv) < 2:
        sys.exit("usage: launchd-disabled.py [--undo] LABEL... ROOT")
    *labels, root = argv
    if not os.path.isdir(os.path.join(root, "System", "Library")):
        sys.exit("error: %s is not the root of a macOS system volume" % root)
    path = os.path.join(root, "private", "var", "db", "com.apple.xpc.launchd", "disabled.plist")
    d, fmt = {}, plistlib.FMT_XML
    if os.path.exists(path):
        raw = open(path, "rb").read()
        fmt = plistlib.FMT_BINARY if raw.startswith(b"bplist") else plistlib.FMT_XML
        d = plistlib.loads(raw)
    for label in labels:
        if undo:
            d.pop(label, None)
        else:
            d[label] = True
    if undo and not d:
        # nothing is disabled: launchd reads a missing file the same way, and this leaves --undo nothing behind
        try:
            os.unlink(path)
        except FileNotFoundError:
            pass
    else:
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            plistlib.dump(d, f, fmt=fmt)
    print("disabled.plist:", sorted(k for k, v in d.items() if v))


if __name__ == "__main__":
    main(sys.argv[1:])
