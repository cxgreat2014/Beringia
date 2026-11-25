#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Concatenate and decode pbzx payload chunks (payload.000, payload.001, ...) into one stream.

  decode-pbzx.py CHUNK_DIR OUTPUT

pbzx: "pbzx", u64 flags, then frames of (u64 raw size, u64 stored size, data) in big endian; a frame is XZ when it
starts with the XZ magic, stored otherwise. The decoded stream of a macOS OTA payload is a YAA1 Apple Archive.
"""
import lzma
import sys
from pathlib import Path


def main(src, out):
    frames = 0
    with open(out, 'wb') as o:
        for part in sorted(Path(src).glob('payload.[0-9][0-9][0-9]')):
            data = part.read_bytes()
            if data[:4] != b'pbzx':
                raise SystemExit(f'{part.name}: not pbzx')
            off = 12
            while off < len(data):
                raw = int.from_bytes(data[off:off + 8], 'big')
                size = int.from_bytes(data[off + 8:off + 16], 'big')
                frame = data[off + 16:off + 16 + size]
                if len(frame) != size:
                    raise SystemExit(f'{part.name}: truncated frame at {off:#x}')
                if frame.startswith(b'\xfd7zXZ\x00'):
                    frame = lzma.decompress(frame)
                if len(frame) != raw:
                    raise SystemExit(f'{part.name}: frame at {off:#x} decodes to {len(frame)}, expected {raw}')
                o.write(frame)
                frames += 1
                off += 16 + size
            print(f'{part.name}: output {o.tell()} bytes', flush=True)
    print(f'{frames} frames -> {out}')


if __name__ == '__main__':
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    main(sys.argv[1], sys.argv[2])
