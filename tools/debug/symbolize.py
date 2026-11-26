#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Symbolize slid XNU addresses in a log (e.g. a "BERINGIA dump" thread dump or a panic backtrace).
#   tools/debug/symbolize.py KERNEL [--slide 0x20000000] < log
# KERNEL is the unstripped kernel (kernel.development.unstripped). Every 0xffffff80... number in the input is
# annotated with symbol+offset (functions and data, from llvm-nm), after subtracting the slide.
import argparse, bisect, os, re, subprocess, sys

ap = argparse.ArgumentParser()
ap.add_argument('kernel')
ap.add_argument('--slide', type=lambda s: int(s, 0), default=0x20000000)
ap.add_argument('--nm', default=os.environ.get('NM', '/usr/lib/llvm-21/bin/llvm-nm'))
a = ap.parse_args()

syms = []
for line in subprocess.run([a.nm, '--defined-only', a.kernel], capture_output=True, text=True, check=True).stdout.splitlines():
    p = line.split()
    if len(p) >= 3 and p[1].lower() in 'tdbs':
        try:
            syms.append((int(p[0], 16), p[2]))
        except ValueError:
            pass
syms.sort()
addrs = [s[0] for s in syms]


def name(v):
    u = v - a.slide
    i = bisect.bisect_right(addrs, u) - 1
    if i < 0 or u - addrs[i] > 0x100000:
        return None
    return f'{syms[i][1]}+{u - addrs[i]:#x}'


def sub(m):
    v = int(m.group(0), 16)
    n = name(v)
    return f'{m.group(0)}<{n}>' if n else m.group(0)


for line in sys.stdin:
    sys.stdout.write(re.sub(r'0xffffff80[0-9a-f]{8}', sub, line))
