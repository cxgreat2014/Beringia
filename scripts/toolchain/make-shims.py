#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Configure macOS-DroidVM's Darwin tool adapter (darwin-tool.py) for the x86_64-apple-darwin toolchain.

  make-shims.py TOOLCHAIN DROIDVM SDK LLVM_BIN

Creates TOOLCHAIN/binx86/{xcrun,target-clang,target-clang++,target-iig,plutil,ditto} -> darwin-tool.py and the
linux-toolchain.json it reads.
"""
import hashlib
import json
import shutil
import sys
import urllib.request
import zipfile
from pathlib import Path

WHEEL_URL = ('https://files.pythonhosted.org/packages/b5/29/c8212e5b3f7e0f164b425e19f0b58780751d744773805dc35c5b16487a30/'
             'openstep_parser-2.0.3-py3-none-any.whl')
WHEEL_SHA256 = 'f219954501e92ced2d3589781201786a930e9a8738128f193b4a425bbee517be'


def main(tc, dv, sdk, llvm):
    tc, dv, sdk, llvm = Path(tc), Path(dv), Path(sdk), Path(llvm)
    binpath, deps = tc / 'binx86', tc / 'python-deps'
    binpath.mkdir(parents=True, exist_ok=True)
    deps.mkdir(parents=True, exist_ok=True)
    wheel = tc / WHEEL_URL.rsplit('/', 1)[-1]
    if not wheel.exists() or hashlib.sha256(wheel.read_bytes()).hexdigest() != WHEEL_SHA256:
        wheel.write_bytes(urllib.request.urlopen(WHEEL_URL, timeout=120).read())
        if hashlib.sha256(wheel.read_bytes()).hexdigest() != WHEEL_SHA256:
            raise SystemExit('openstep_parser wheel: checksum mismatch')
    with zipfile.ZipFile(wheel) as z:
        z.extractall(deps)
    tool = dv / 'compat/xnu-linux/linux/darwin-tool.py'
    for name in ('xcrun', 'target-clang', 'target-clang++', 'target-iig', 'plutil', 'ditto'):
        p = binpath / name
        if p.exists() or p.is_symlink():
            p.unlink()
        p.symlink_to(tool)
    prefix = tc / 'toolchain'
    tools = {'clang': str(binpath / 'target-clang'), 'clang++': str(binpath / 'target-clang++'),
             'mig': str(prefix / 'bin/mig'), 'migcom': str(prefix / 'bin/migcom'),
             'dsymutil': str(llvm / 'dsymutil')}
    for name in ('ar', 'nm', 'lipo', 'libtool', 'otool', 'strip', 'nmedit'):
        tools[name] = str(prefix / 'bin' / ('x86_64-apple-darwin-' + name))
    for name in ('git', 'cmake', 'ninja', 'unifdef', 'python3'):
        tools[name] = shutil.which(name)
    settings = json.loads((sdk / 'SDKSettings.json').read_text())
    config = dict(sdk=str(sdk), sdk_version=settings['Version'], clang=str(llvm / 'clang'), clangxx=str(llvm / 'clang++'),
                  ld=str(prefix / 'bin/x86_64-apple-darwin-ld'), tools=tools, python_deps=str(deps),
                  sdk_settings_sha256=hashlib.sha256((sdk / 'SDKSettings.json').read_bytes()).hexdigest(),
                  native_compile=True, drop_args=['-mno-implicit-sme'],
                  extra_args=['-fno-caret-diagnostics', '-fno-diagnostics-show-note-include-stack',
                              '-Wno-reserved-identifier', '-Wno-unsafe-buffer-usage'])
    (binpath / 'linux-toolchain.json').write_text(json.dumps(config, indent=2) + '\n')
    print('shims ready:', binpath)


if __name__ == '__main__':
    if len(sys.argv) != 5:
        raise SystemExit(__doc__)
    main(*sys.argv[1:])
