# SPDX-License-Identifier: GPL-2.0-only
"""Just enough x86_64 Mach-O for tools/kc: slices, load commands, segments, symbol tables."""
import os
import plistlib
import struct
import sys

PAGE = 0x1000
MH_MAGIC_64 = 0xFEEDFACF
CPU_TYPE_X86_64 = 0x01000007
MH_EXECUTE, MH_KEXT_BUNDLE = 2, 0xB
LC_SYMTAB, LC_DYSYMTAB, LC_SEGMENT_64, LC_UUID, LC_CODE_SIGNATURE = 0x2, 0xB, 0x19, 0x1B, 0x1D
SEG = struct.Struct('<II16sQQQQiiII')       # segment_command_64
SECT = struct.Struct('<16s16sQQIIIIIIII')   # section_64
SYMTAB = struct.Struct('<IIIIII')           # symtab_command
NLIST = struct.Struct('<IBBHQ')             # nlist_64
N_STAB, N_TYPE, N_EXT = 0xE0, 0x0E, 0x01
N_UNDF, N_ABS, N_SECT, N_INDR = 0x0, 0x2, 0xE, 0xA


def align(x, a=PAGE):
    return (x + a - 1) & ~(a - 1)


def die(msg):
    sys.exit(f'{os.path.basename(sys.argv[0])}: {msg}')


def thin_x86_64(data, name):
    """(x86_64 Mach-O bytes, filetype) from a thin or fat file."""
    magic = struct.unpack_from('>I', data)[0]
    if magic in (0xCAFEBABE, 0xCAFEBABF):
        n = struct.unpack_from('>I', data, 4)[0]
        wide = magic == 0xCAFEBABF
        for i in range(n):
            if wide:
                cpu, _, off, size, _, _ = struct.unpack_from('>iiQQII', data, 8 + 32 * i)
            else:
                cpu, _, off, size, _ = struct.unpack_from('>iiIII', data, 8 + 20 * i)
            if cpu == CPU_TYPE_X86_64:
                data = data[off:off + size]
                break
        else:
            die(f'{name}: no x86_64 slice')
    magic, cpu, _, ftype = struct.unpack_from('<IiiI', data)
    if magic != MH_MAGIC_64 or cpu != CPU_TYPE_X86_64:
        die(f'{name}: not an x86_64 Mach-O')
    return bytes(data), ftype


def load_commands(data):
    """(cmd, offset, size) for each load command."""
    ncmds = struct.unpack_from('<I', data, 16)[0]
    off = 32
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<II', data, off)
        yield cmd, off, size
        off += size


def find_command(data, which):
    for cmd, off, size in load_commands(data):
        if cmd == which:
            return off, size
    return None


def segments(data):
    """{name: (load command offset, [segment_command_64 fields])}."""
    out = {}
    for cmd, off, _ in load_commands(data):
        if cmd == LC_SEGMENT_64:
            f = list(SEG.unpack_from(data, off))
            out[f[2].rstrip(b'\0').decode()] = (off, f)
    return out


def symbols(data):
    """[(index, name, n_type, n_sect, n_desc, n_value, indirect target or None)] of the LC_SYMTAB."""
    lc = find_command(data, LC_SYMTAB)
    if not lc:
        return []
    _, _, symoff, nsyms, stroff, strsize = SYMTAB.unpack_from(data, lc[0])
    strtab = data[stroff:stroff + strsize]

    def name(x):
        return strtab[x:strtab.index(b'\0', x)].decode()

    out = []
    for i in range(nsyms):
        strx, typ, sect, desc, value = NLIST.unpack_from(data, symoff + i * NLIST.size)
        indr = name(value) if (typ & N_TYPE) == N_INDR and not typ & N_STAB else None
        out.append((i, name(strx), typ, sect, desc, value, indr))
    return out


def kext_executable(bundle):
    """(Info.plist dict, executable path or None) of a kext bundle (flat or Contents/ layout)."""
    for info, macos in ((os.path.join(bundle, 'Contents', 'Info.plist'), os.path.join(bundle, 'Contents', 'MacOS')),
                        (os.path.join(bundle, 'Info.plist'), bundle)):
        if os.path.isfile(info):
            d = plistlib.load(open(info, 'rb'))
            exe = d.get('CFBundleExecutable')
            path = os.path.join(macos, exe) if exe else None
            return d, (path if path and os.path.isfile(path) else None)
    return None, None
