#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Adapt pre-generated DriverKit iig output to xnu-12377.1.9.

Apple's iig compiler is not public, so the build reuses iig output generated for a later XNU (see
scripts/xnu/build.sh). Dispatchers for RPCs that xnu-12377.1.9 does not implement are replaced by
KERN_NOT_SUPPORTED, which is what a kernel without the method answers.
"""
import sys
from pathlib import Path

MISSING = [
    ('IOUserServer', 'Panic'),
]

def main(objdir):
    count = 0
    for p in Path(objdir).rglob('*.iig.cpp'):
        s = p.read_text()
        orig = s
        for cls, method in MISSING:
            old = (f'ret = {cls}::{method}_Invoke(rpc, self, SimpleMemberFunctionCast({cls}::{method}_Handler, '
                   f'*self, &{cls}::{method}_Impl));')
            s = s.replace(old, f'ret = KERN_NOT_SUPPORTED; /* {cls}::{method} is not in xnu-12377.1.9 */')
        if s != orig:
            p.write_text(s)
            count += 1
    print(f'iig fixup: {count} file(s) adapted')

if __name__ == '__main__':
    main(sys.argv[1])
