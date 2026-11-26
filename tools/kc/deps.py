#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Compute a kext dependency closure and its load order from a KDK's /System/Library/Extensions.

  tools/kc/deps.py EXTENSIONS BUNDLE_ID...

Prints one bundle path per line, relative to EXTENSIONS, dependencies before dependents; that is the order
scripts/kc/build.sh links them in. Like XNU (OSKext::getPropertyForHostArch), a kext's OSBundleLibraries_x86_64
replaces OSBundleLibraries on x86_64: Apple's security kexts drop their Apple silicon dependencies there.
com.apple.kpi.* pseudo-extensions are provided by the kernel (skeleton.py) and are not listed.
Dependencies missing from EXTENSIONS, and kexts without an x86_64 executable, are reported on stderr.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from kpibind import bundle_index  # noqa: E402
from macho import die, kext_executable, thin_x86_64  # noqa: E402


def closure(root_dir, index, roots):
    """(ordered relative paths, problems) for the closure of roots (bundle identifiers)."""
    order, problems, done, visiting = [], [], set(), set()

    def visit(ident):
        if ident in done or ident.startswith('com.apple.kpi.'):
            return
        if ident in visiting:
            problems.append(f'dependency cycle at {ident}')
            return
        visiting.add(ident)
        if ident not in index:
            problems.append(f'{ident}: not in the extensions directory')
        else:
            path = index[ident]
            info, exe = kext_executable(path)
            libs = info.get('OSBundleLibraries_x86_64') or info.get('OSBundleLibraries') or {}
            for dep in sorted(libs):
                visit(dep)
            rel = os.path.relpath(path, root_dir)
            if exe:
                try:
                    thin_x86_64(open(exe, 'rb').read(), exe)
                except SystemExit:
                    problems.append(f'{ident}: no x86_64 executable ({rel})')
            order.append(rel)
        visiting.discard(ident)
        done.add(ident)

    for r in roots:
        visit(r)
    return order, problems


def main():
    if len(sys.argv) < 3:
        die('usage: deps.py EXTENSIONS BUNDLE_ID...')
    root_dir = sys.argv[1]
    order, problems = closure(root_dir, bundle_index(root_dir), sys.argv[2:])
    for p in problems:
        print(f'deps: {p}', file=sys.stderr)
    for rel in order:
        print(rel)
    print(f'deps: {len(order)} kexts, {len(problems)} problems', file=sys.stderr)


if __name__ == '__main__':
    main()
