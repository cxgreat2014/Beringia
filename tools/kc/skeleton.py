#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Turn a built XNU kernel into an empty legacy (kextcache-style, pre-macOS 11) x86_64 prelinked kernel.

  tools/kc/skeleton.py KERNEL KPI_DIR OUT

KERNEL   the kernel Mach-O (MH_EXECUTE). XNU's link already gives it empty __PRELINK_TEXT and __PRELINK_INFO segments.
KPI_DIR  the KDK's System.kext/PlugIns: the kernel's pseudo-extensions (com.apple.kpi.*, IONVRAMFamily, ...).
OUT      the prelinked kernel; kcinject appends kexts to it.

The layout is the one 10.15's prelinkedkernel has and OpenCorePkg's legacy injection path expects:

  __TEXT ... __LINKEDIT | __PRELINK_TEXT | __PRELINK_INFO        (file and memory order; each starts on a page)

- __PRELINK_TEXT starts at the first page after the kernel, in the file and in memory, and holds the KPI symbol sets
  (Mach-Os with only a symbol table: the KPI names and aliases such as _IOMalloc -> _IOMalloc_external, which a
  linker needs). XNU registers them as interface kexts.
- __PRELINK_INFO, last in the file, holds the plist XNU reads in readPrelinkedExtensions(): a <dict> with
  _PrelinkInfoDictionary, one entry per kext (its Info.plist plus where its executable is), and _PrelinkKCID.

Every executable has two addresses: its source address in __PRELINK_TEXT and its load address in the kext basement,
where XNU copies it at boot. They are consecutive in both, so kcinject (OpenCorePkg) appends the next kext at the
end of each. Addresses in the plist are unslid; XNU adds vm_kernel_slide, as for any prelinked kernel.
"""
import base64
import hashlib
import os
import plistlib
import struct
import sys
from xml.sax.saxutils import escape

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from macho import CPU_TYPE_X86_64, MH_EXECUTE, MH_KEXT_BUNDLE, MH_MAGIC_64, SECT, SEG, align, die, segments, thin_x86_64


def set_segment(buf, segs, name, vmaddr, size, fileoff, sectname):
    """Place an (until now empty) segment and its one section."""
    if name not in segs:
        die(f'kernel has no {name} segment (not an XNU kernel?)')
    off, f = segs[name]
    if f[9] != 1:
        die(f'{name} has {f[9]} sections, expected 1')
    f[3], f[4], f[5], f[6] = vmaddr, size, fileoff, size
    SEG.pack_into(buf, off, *f)
    s = list(SECT.unpack_from(buf, off + SEG.size))
    if s[0].rstrip(b'\0').decode() != sectname:
        die(f'{name} section is {s[0]!r}, expected {sectname}')
    s[2], s[3], s[4] = vmaddr, size, fileoff
    SECT.pack_into(buf, off + SEG.size, *s)


def xml(v, indent):
    """Serialize a plist value the way XNU's OSUnserializeXML reads prelink info."""
    pad = '\t' * indent
    if isinstance(v, bool):
        return pad + ('<true/>' if v else '<false/>')
    if isinstance(v, Hex):
        return f'{pad}<integer size="64">{v:#x}</integer>'
    if isinstance(v, int):
        return f'{pad}<integer>{v}</integer>'
    if isinstance(v, str):
        return f'{pad}<string>{escape(v)}</string>'
    if isinstance(v, bytes):
        return f'{pad}<data>{base64.b64encode(v).decode()}</data>'
    if isinstance(v, dict):
        if not v:
            return pad + '<dict/>'
        body = ''.join(f'{pad}\t<key>{escape(k)}</key>\n{xml(x, indent + 1)}\n' for k, x in v.items())
        return f'{pad}<dict>\n{body}{pad}</dict>'
    if isinstance(v, list):
        if not v:
            return pad + '<array/>'
        return f'{pad}<array>\n' + ''.join(xml(x, indent + 1) + '\n' for x in v) + f'{pad}</array>'
    die(f'cannot serialize {type(v).__name__} in an Info.plist')


class Hex(int):
    """A 64-bit address, written in hex like kextcache does."""


def main():
    if len(sys.argv) != 4:
        die('usage: skeleton.py KERNEL KPI_DIR OUT')
    kernel_path, kpi_dir, out_path = sys.argv[1:]
    kernel = bytearray(open(kernel_path, 'rb').read())
    magic, cpu, _, ftype = struct.unpack_from('<IiiI', kernel)
    if magic != MH_MAGIC_64 or cpu != CPU_TYPE_X86_64 or ftype != MH_EXECUTE:
        die(f'{kernel_path}: not an x86_64 kernel Mach-O (MH_EXECUTE)')
    segs = segments(kernel)
    for name in ('__PRELINK_TEXT', '__PRELINK_INFO'):
        if name in segs and segs[name][1][4]:
            die(f'{kernel_path} is already prelinked ({name} is not empty)')

    # Where the kernel ends, in memory and in the file.
    vm_end = align(max(f[3] + f[4] for _, f in segs.values()))
    file_end = align(len(kernel))
    base = segs['__TEXT'][1][3] - segs['__TEXT'][1][5]
    # The kext basement (osfmk/kern/kext_alloc.c, CONFIG_KEXT_BASEMENT): 2 GB below the end of __TEXT, so kext code
    # reaches the kernel with 32-bit displacements. Prelinked kexts run there: XNU allocates each one at its
    # _PrelinkExecutableLoadAddr in the kext map and copies it from _PrelinkExecutableSourceAddr (in __PRELINK_TEXT,
    # freed afterwards); post-boot kexts start after __PRELINK_TEXT's size.
    load_base = align(segs['__TEXT'][1][3] + segs['__TEXT'][1][4]) - 0x80000000

    # The pseudo-extensions: symbol sets go into __PRELINK_TEXT, codeless ones are plist-only.
    entries, text = [], bytearray()
    for bundle in sorted(os.listdir(kpi_dir)):
        path = os.path.join(kpi_dir, bundle)
        info_path = os.path.join(path, 'Info.plist')
        if not bundle.endswith('.kext') or not os.path.isfile(info_path):
            continue
        info = plistlib.load(open(info_path, 'rb'))
        if 'IOKitPersonalities' in info and not info['IOKitPersonalities']:
            del info['IOKitPersonalities']
        info['_PrelinkBundlePath'] = f'/System/Library/Extensions/System.kext/PlugIns/{bundle}'
        exe = info.get('CFBundleExecutable')
        if exe and os.path.isfile(os.path.join(path, exe)):
            image, ftype = thin_x86_64(open(os.path.join(path, exe), 'rb').read(), f'{bundle}/{exe}')
            if ftype != MH_KEXT_BUNDLE:
                die(f'{bundle}/{exe}: not a kext bundle Mach-O')
            size = align(len(image))
            info['_PrelinkExecutableRelativePath'] = exe
            info['_PrelinkExecutableSourceAddr'] = Hex(vm_end + len(text))
            info['_PrelinkExecutableLoadAddr'] = Hex(load_base + len(text))
            text += image + bytes(size - len(image))
            info['_PrelinkExecutableSize'] = Hex(size)
            info['_PrelinkKmodInfo'] = Hex(0)
        entries.append(info)
    if not any('_PrelinkExecutableLoadAddr' in e for e in entries):
        die(f'{kpi_dir}: no KPI symbol sets (expected the KDK System.kext/PlugIns)')

    kcid = hashlib.sha256(bytes(kernel) + bytes(text)).digest()[:16]
    plist = xml({'_PrelinkInfoDictionary': entries, '_PrelinkKCID': kcid}, 0) + '\n'
    info = plist.encode() + b'\0'
    info += bytes(align(len(info)) - len(info))

    set_segment(kernel, segs, '__PRELINK_TEXT', vm_end, len(text), file_end, '__text')
    set_segment(kernel, segs, '__PRELINK_INFO', vm_end + len(text), len(info), file_end + len(text), '__info')
    out = kernel + bytes(file_end - len(kernel)) + text + info
    with open(out_path, 'wb') as f:
        f.write(out)
    print(f'skeleton: {out_path}: {len(entries)} pseudo-extensions; __PRELINK_TEXT {vm_end:#x}+{len(text):#x} '
          f'(file {file_end:#x}, delta {file_end - (vm_end - base):#x}), kexts run from {load_base:#x}; '
          f'__PRELINK_INFO {len(info):#x} bytes')


if __name__ == '__main__':
    main()
