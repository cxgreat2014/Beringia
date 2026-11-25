# Notices

## Upstream code Beringia builds or patches (fetched at pinned revisions, never committed)

| Project | License | Use |
|---|---|---|
| [XNU](https://github.com/apple-oss-distributions/xnu) `xnu-12377.1.9` | APSL-2.0 | The hosted kernel; `xnu/patches/` modify it |
| [macOS-DroidVM](https://github.com/Droid-VM/macOS-DroidVM) | see upstream | Linux-hosted Darwin toolchain support, XNU Linux build support (adapted in `xnu/patches/build/0001`), pre-generated iig output |
| [cctools-port](https://github.com/tpoechtrager/cctools-port), bootstrap_cmds (MIG) | APSL-2.0 | Mach-O linker and tools, MIG |
| LLVM | Apache-2.0 WITH LLVM-exception | Compiler |
| Linux (microsoft/WSL2-Linux-Kernel) | GPL-2.0 | The host kernel |

## Reference project

[xnulinux](https://github.com/AFK-surf/xnulinux) (Copyright (c) 2026 Heyang Zhou and contributors; GPL-2.0-only /
APSL-2.0 / others per file) runs arm64 XNU as a Linux kernel module. Beringia is an independent x86_64 project that
follows its architecture. Any file adapted from xnulinux states so in its header and keeps the original license and
copyright notice.

## Apple software

No Apple binaries are part of Beringia. macOS, its kernel collections, kexts, trust caches, SDKs and Kernel Debug Kits
are Apple's; users supply their own. macOS is a trademark of Apple Inc. Beringia is not affiliated with Apple.
