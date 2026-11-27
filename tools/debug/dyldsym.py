#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Symbolize user-space addresses that point into macOS's dyld shared cache (x86_64).
#
#   tools/debug/dyldsym.py CACHE --slide SLIDE ADDR...       e.g. --slide 0x3a4000 0x7ff802eba5c6
#   tools/debug/dyldsym.py CACHE < dump.txt                  annotate a "BERINGIA dump" (beringia_xnu.ko dump=NAME)
#
# CACHE is the main cache file (…/System/Library/dyld/dyld_shared_cache_x86_64h); its subcaches (.01, .02, …) and
# .map must be next to it. In dump mode the slide is taken from each "task image … cache_slide …" line. Names come from
# the images' symbol tables in the cache (exported and other global symbols; the local symbols live in a .symbols
# file Apple does not ship in the OTA), so a frame inside a static function shows the nearest preceding global symbol:
# read "+offset" with that in mind. Standard library only; run it with python3 -I.
import argparse
import bisect
import mmap
import os
import re
import struct
import sys

LC_SEGMENT_64 = 0x19
LC_SYMTAB = 0x2


class Cache:
    def __init__(self, path):
        self.files = []        # (path, mmap)
        self.mappings = []     # (address, size, file index, file offset)
        base = path
        names = [base] + sorted(p for p in (os.path.join(os.path.dirname(base), n)
                                            for n in os.listdir(os.path.dirname(base) or "."))
                                if re.fullmatch(re.escape(base) + r"\.\d+", p))
        for i, p in enumerate(names):
            f = open(p, "rb")
            m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
            if not m[:4] == b"dyld":
                raise SystemExit("%s: not a dyld shared cache" % p)
            self.files.append((p, m))
            moff, mcount = struct.unpack_from("<II", m, 0x10)
            for j in range(mcount):
                addr, size, foff, _maxp, _initp = struct.unpack_from("<QQQII", m, moff + 32 * j)
                self.mappings.append((addr, size, i, foff))
        self.mappings.sort()
        self.starts = [m[0] for m in self.mappings]
        self.images = self._read_map(base + ".map")
        self.image_starts = [s for s, _e, _p in self.images]
        self.symtabs = {}

    @staticmethod
    def _read_map(path):
        """[(start, end, image path)] of every image's __TEXT, from the .map dyld writes next to the cache."""
        out = []
        image = None
        with open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                if line.startswith("/"):
                    image = line.strip()
                    continue
                m = re.match(r"\s+__TEXT\s+0x([0-9A-Fa-f]+)\s*->\s*0x([0-9A-Fa-f]+)", line)
                if m and image:
                    out.append((int(m.group(1), 16), int(m.group(2), 16), image))
        out.sort()
        return out

    def read(self, addr, n):
        i = bisect.bisect_right(self.starts, addr) - 1
        if i < 0:
            return None
        a, size, fi, foff = self.mappings[i]
        if addr + n > a + size:
            return None
        m = self.files[fi][1]
        return m[foff + addr - a:foff + addr - a + n]

    def image_of(self, addr):
        i = bisect.bisect_right(self.image_starts, addr) - 1
        if i >= 0 and self.images[i][0] <= addr < self.images[i][1]:
            return self.images[i]
        return None

    def symbols(self, start):
        """Sorted [(address, name)] from the symbol table of the image whose mach header is at start."""
        if start in self.symtabs:
            return self.symtabs[start]
        syms = []
        hdr = self.read(start, 32)
        if hdr and struct.unpack_from("<I", hdr)[0] == 0xfeedfacf:
            ncmds, sizeofcmds = struct.unpack_from("<II", hdr, 16)
            cmds = self.read(start + 32, sizeofcmds) or b""
            linkedit = symtab = None
            off = 0
            for _ in range(ncmds):
                cmd, size = struct.unpack_from("<II", cmds, off)
                if cmd == LC_SEGMENT_64 and cmds[off + 8:off + 24].rstrip(b"\0") == b"__LINKEDIT":
                    vmaddr, _vmsize, fileoff = struct.unpack_from("<QQQ", cmds, off + 24)
                    linkedit = (vmaddr, fileoff)
                elif cmd == LC_SYMTAB:
                    symtab = struct.unpack_from("<IIII", cmds, off + 8)
                off += size
            if linkedit and symtab:
                symoff, nsyms, stroff, strsize = symtab
                to_va = lambda o: linkedit[0] + o - linkedit[1]
                nl = self.read(to_va(symoff), nsyms * 16) or b""
                strs = self.read(to_va(stroff), strsize) or b""
                for k in range(len(nl) // 16):
                    strx, ntype, sect, _desc, value = struct.unpack_from("<IBBHQ", nl, 16 * k)
                    if ntype & 0xe0 or not sect or not value:     # stabs, undefined
                        continue
                    end = strs.find(b"\0", strx)
                    syms.append((value, strs[strx:end].decode("utf-8", "replace")))
        syms.sort()
        self.symtabs[start] = syms
        return syms

    def symbolize(self, addr, slide):
        u = addr - slide
        img = self.image_of(u)
        if not img:
            return None
        start, _end, path = img
        syms = self.symbols(start)
        i = bisect.bisect_right([s for s, _ in syms], u) - 1
        lib = os.path.basename(path)
        if i >= 0:
            return "%s`%s+%#x" % (lib, syms[i][1], u - syms[i][0])
        return "%s+%#x" % (lib, u - start)


def main():
    p = argparse.ArgumentParser(description="Symbolize user addresses in the dyld shared cache")
    p.add_argument("cache")
    p.add_argument("--slide", type=lambda s: int(s, 0))
    p.add_argument("addr", nargs="*", type=lambda s: int(s, 0))
    a = p.parse_args()
    c = Cache(a.cache)
    if a.addr:
        for x in a.addr:
            print("%#x %s" % (x, c.symbolize(x, a.slide or 0) or "?"))
        return
    slide = image = None
    for line in sys.stdin:
        line = line.rstrip("\n")
        m = re.search(r"task image (0x[0-9a-f]+) cache_slide (0x[0-9a-f]+)", line)
        if m:
            image, slide = int(m.group(1), 16), int(m.group(2), 16)
        elif re.match(r"pid \d+ proc ", line.strip()):
            # a thread's line starts its task's block: until a "task image" line follows, this task's addresses
            # must not be symbolized with the previous task's image and slide
            image = slide = None
        print(line)
        if "user rip" in line and slide is not None:
            for tok in re.findall(r"0x[0-9a-f]+", line.split(" bt ", 1)[0].split("rsp")[0]) + \
                    re.findall(r"0x[0-9a-f]+", line.split(" bt ", 1)[1] if " bt " in line else ""):
                x = int(tok, 16)
                if not x:
                    continue
                s = c.symbolize(x, slide)
                if s is None and image and image <= x < image + (64 << 20):
                    s = "main+%#x" % (x - image)
                print("      %s %s" % (tok, s or "?"))


if __name__ == "__main__":
    main()
