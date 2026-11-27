#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Save the hosted XNU's framebuffer (/dev/beringia-fb, beringia_xnu.ko fb=WxH) as a PNG. Standard library only.
#
#   sudo tools/debug/fbshot.py [OUT.png]          default: fb-YYYYmmdd-HHMMSS.png in the current directory
#   sudo tools/debug/fbshot.py --every 5 DIR      one screenshot every 5 seconds into DIR, until interrupted
#
# The device is root-only (it shows whatever macOS displays). Pixels are XRGB8888 (bytes B, G, R, X).
import argparse
import os
import struct
import sys
import time
import zlib

DEV = "/dev/beringia-fb"
GEOMETRY = "/sys/class/misc/beringia-fb/geometry"


def geometry():
    with open(GEOMETRY) as f:
        w, h, stride, fmt = f.read().split()
    if fmt != "xrgb8888":
        raise SystemExit("unsupported pixel format %s" % fmt)
    return int(w), int(h), int(stride)


def grab():
    w, h, stride = geometry()
    with open(DEV, "rb", buffering=0) as f:
        raw = b""
        while len(raw) < stride * h:
            chunk = f.read(stride * h - len(raw))
            if not chunk:
                raise SystemExit("short read from %s" % DEV)
            raw += chunk
    return w, h, stride, raw


def png(w, h, stride, raw):
    rows = bytearray()
    for y in range(h):
        line = raw[y * stride:y * stride + w * 4]
        rgb = bytearray(w * 3)
        rgb[0::3] = line[2::4]
        rgb[1::3] = line[1::4]
        rgb[2::3] = line[0::4]
        rows += b"\0" + rgb

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xffffffff)

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(bytes(rows), 6)) + chunk(b"IEND", b""))


def save(path):
    w, h, stride, raw = grab()
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(png(w, h, stride, raw))
    os.replace(tmp, path)
    return w, h


def main():
    p = argparse.ArgumentParser(description="Save the hosted XNU framebuffer as PNG")
    p.add_argument("out", nargs="?", help="PNG file, or a directory with --every")
    p.add_argument("--every", type=float, metavar="SECONDS", help="keep taking screenshots at this interval")
    a = p.parse_args()
    stamp = lambda: time.strftime("fb-%Y%m%d-%H%M%S.png")
    if a.every:
        d = a.out or "."
        os.makedirs(d, exist_ok=True)
        try:
            while True:
                path = os.path.join(d, stamp())
                w, h = save(path)
                print(path, "%dx%d" % (w, h), flush=True)
                time.sleep(a.every)
        except KeyboardInterrupt:
            return
    path = a.out or stamp()
    w, h = save(path)
    print(path, "%dx%d" % (w, h))


if __name__ == "__main__":
    main()
