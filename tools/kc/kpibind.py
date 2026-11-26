#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Bind a kext's imports the way Apple's kext linker does: through the dependencies its Info.plist declares.

  tools/kc/kpibind.py [--strict] KEXT INFO_PLIST EXTENSIONS OUT

KEXT is a thin x86_64 kext executable, EXTENSIONS the KDK's /System/Library/Extensions (it holds the dependencies,
and the KPI symbol sets in System.kext/PlugIns).

Why: a KPI symbol set (com.apple.kpi.private, ...) can export a name as an alias of another kernel function
(config/*.exports in XNU, e.g. _kfree_type_impl -> _kfree_type_impl_external). XNU's link applies these aliases to
the kernel's own symbol table, so for most of them the kernel symbol with the KPI name already is the alias target.
Not for all: in xnu-12377.1.9, _kalloc_type_var_impl and _kfree_type_var_impl (2 of the KDK's 110 aliases) are also
internal kernel functions at different addresses, which expect a kalloc_type view processed at startup (a kext's is
not). OpenCorePkg's linker looks a name up in the kernel before the KPIs and would bind a kext to the internal
function. This tool renames every aliased import to its target first; the linker then finds the right function.

It also reports imports that no declared dependency exports (Apple's linker would refuse them; OpenCorePkg would
silently bind them to any kernel symbol of that name). --strict makes that an error.

The kext's code signature is dropped (the string table grows into its place; prelinked kexts are not verified
against it, and linking invalidates it anyway).
"""
import os
import plistlib
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from macho import (LC_CODE_SIGNATURE, LC_DYSYMTAB, LC_SYMTAB, N_ABS, N_EXT, N_SECT, N_STAB, N_TYPE, N_UNDF, NLIST,
                   SEG, SYMTAB, align, die, find_command, kext_executable, load_commands, segments, symbols,
                   thin_x86_64)


def bundle_index(extensions):
    """{bundle identifier: bundle path} for every kext under EXTENSIONS (including PlugIns)."""
    index = {}
    for top, dirs, _ in os.walk(extensions):
        for d in list(dirs):
            if d.endswith('.kext'):
                path = os.path.join(top, d)
                info, _ = kext_executable(path)
                if info and 'CFBundleIdentifier' in info:
                    index.setdefault(info['CFBundleIdentifier'], path)
    return index


def exports(bundle):
    """(names, {alias: target}) a dependency provides to kexts that link against it."""
    info, exe = kext_executable(bundle)
    if not exe:
        return set(), {}
    data, _ = thin_x86_64(open(exe, 'rb').read(), exe)
    names, aliases = set(), {}
    symbol_set = info.get('OSKernelResource') is True
    for _, name, typ, _, _, _, indr in symbols(data):
        if typ & N_STAB:
            continue
        if indr:
            aliases[name] = indr
            names.add(name)
        elif symbol_set or (typ & N_EXT and (typ & N_TYPE) in (N_SECT, N_ABS)):
            names.add(name)     # a symbol set lists the kernel symbols it exports as undefined
    return names, aliases


def drop_code_signature(buf):
    lc = find_command(buf, LC_CODE_SIGNATURE)
    if not lc:
        return None
    off, size = lc
    dataoff, datasize = struct.unpack_from('<II', buf, off + 8)
    ncmds, sizeofcmds = struct.unpack_from('<II', buf, 16)
    end = 32 + sizeofcmds
    buf[off:end] = buf[off + size:end] + bytes(size)
    struct.pack_into('<II', buf, 16, ncmds - 1, sizeofcmds - size)
    return dataoff, datasize


def main():
    args = sys.argv[1:]
    strict = '--strict' in args
    args = [a for a in args if a != '--strict']
    if len(args) != 4:
        die('usage: kpibind.py [--strict] KEXT INFO_PLIST EXTENSIONS OUT')
    kext_path, info_path, extensions, out_path = args
    data, _ = thin_x86_64(open(kext_path, 'rb').read(), kext_path)
    info = plistlib.load(open(info_path, 'rb'))
    ident = info.get('CFBundleIdentifier', kext_path)

    index = bundle_index(extensions)
    names, aliases, missing = set(), {}, []
    # XNU (getPropertyForHostArch) prefers the architecture's own dependency list.
    libs = info.get('OSBundleLibraries_x86_64') or info.get('OSBundleLibraries') or {}
    for dep in libs:
        if dep not in index:
            missing.append(dep)
            continue
        n, a = exports(index[dep])
        names |= n
        for k, v in a.items():
            if aliases.setdefault(k, v) != v:
                die(f'{ident}: {k} is an alias of both {aliases[k]} and {v} in its dependencies')

    syms = symbols(data)
    imports = [s for s in syms if (s[2] & N_TYPE) == N_UNDF and s[2] & N_EXT and not s[2] & N_STAB and s[5] == 0]
    rebind = {s[0]: aliases[s[1]] for s in imports if s[1] in aliases}
    unresolved = sorted(s[1] for s in imports if s[1] not in names)

    buf = bytearray(data)
    if rebind:
        lc_off, _ = find_command(buf, LC_SYMTAB)
        _, _, symoff, nsyms, stroff, strsize = SYMTAB.unpack_from(buf, lc_off)
        ends = [symoff + nsyms * NLIST.size, stroff + strsize]
        dy = find_command(buf, LC_DYSYMTAB)
        if dy:
            v = struct.unpack_from('<18I', buf, dy[0] + 8)
            ends += [v[6] + v[7] * 4,  # tocoff, ntoc (approximate entry size is irrelevant: must be empty)
                     v[10] + v[11] * 4, v[12] + v[13] * 4, v[14] + v[15] * 8, v[16] + v[17] * 8]
        sig = drop_code_signature(buf)
        if max(ends) != stroff + strsize:
            die(f'{ident}: the string table is not last in __LINKEDIT; cannot extend it')
        if sig and sig[0] < stroff + strsize:
            die(f'{ident}: the code signature is not after the string table')
        strtab = bytearray(buf[stroff:stroff + strsize])
        for i, target in rebind.items():
            strx = len(strtab)
            strtab += target.encode() + b'\0'
            struct.pack_into('<I', buf, symoff + i * NLIST.size, strx)
        strtab += bytes(align(len(strtab), 8) - len(strtab))
        del buf[stroff:]
        buf += strtab
        SYMTAB.pack_into(buf, lc_off, LC_SYMTAB, SYMTAB.size, symoff, nsyms, stroff, len(strtab))
        off, f = segments(buf)['__LINKEDIT']
        f[6] = len(buf) - f[5]           # filesize
        f[4] = align(f[6])               # vmsize
        SEG.pack_into(buf, off, *f)
    open(out_path, 'wb').write(buf)

    print(f'kpibind: {ident}: {len(imports)} imports, {len(rebind)} through KPI aliases'
          + (f' ({", ".join(f"{syms[i][1]} -> {t}" for i, t in rebind.items())})' if rebind else '')
          + f', {len(unresolved)} not exported by its dependencies')
    for dep in missing:
        print(f'kpibind: {ident}: dependency {dep} is not in {extensions}')
    for name in unresolved:
        print(f'kpibind: {ident}: {name} is not exported by its dependencies')
    if strict and (unresolved or missing):
        sys.exit(1)


if __name__ == '__main__':
    main()
