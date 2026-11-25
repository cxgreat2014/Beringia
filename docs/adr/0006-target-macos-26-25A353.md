<!-- SPDX-License-Identifier: GPL-2.0-only -->
# 0006 — Target macOS 26.0 (25A353) and xnu-12377.1.9

Status: accepted

## Context

macOS 26 is the last macOS with x86_64 support. Userspace and kexts must match the kernel's ABI. The x86_64 kernel in
the 25A353 Kernel Debug Kit identifies itself as `xnu_development-12377.1.9~2`, and the 25A353 boot kernel collection
embeds `xnu-12377.1.9~3/RELEASE_X86_64`. A later 26.x source (`xnu-12377.121.6`) and an older one
(`xnu-11417.140.69`) were tried before and do not match.

## Decision

Build `xnu-12377.1.9` (commit f6217f891ac0bb64f3d375211650a4c1ff8ca1ea) and validate with 25A353 assets only.

## Consequences

- Moving to a later 26.x userspace would need the matching kernel source, once Apple publishes it.
