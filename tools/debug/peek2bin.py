#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Turn "BERINGIA peek:" lines (beringia_xnu.ko dump=peek:PID:ADDR:LEN) into a binary file and print its base address,
# e.g. to disassemble code that is only decrypted in memory:
#
#   dmesg | tools/debug/peek2bin.py out.bin
#   objdump -D -b binary -m i386:x86-64 --adjust-vma=BASE out.bin
#
# Words that were not mapped become 0xcc bytes. Standard library only.
import re
import struct
import sys

out = sys.argv[1]
base = None
data = bytearray()
for line in sys.stdin:
    m = re.search(r"BERINGIA peek: (0x[0-9a-f]+)((?: (?:[0-9a-f]{16}|-))+)\s*$", line)
    if not m:
        continue
    addr = int(m.group(1), 16)
    if base is None:
        base = addr
    if addr != base + len(data):
        sys.exit("gap at %#x" % addr)
    for w in m.group(2).split():
        data += b"\xcc" * 8 if w == "-" else struct.pack("<Q", int(w, 16))
if base is None:
    sys.exit("no BERINGIA peek lines")
open(out, "wb").write(data)
print("%#x %d" % (base, len(data)))
