<!-- SPDX-License-Identifier: GPL-2.0-only -->
# 0001 — XNU manages real, reserved physical memory

Status: accepted

## Context

A hosted XNU builds page tables: its kernel pmap and one per task. If the physical addresses it believes in are not
the machine's, those tables cannot be given to the MMU, and every XNU mapping has to be shadowed into Linux's tables by
software. A previous x86_64 attempt did exactly that (fake physical addresses starting at 32 MiB, mirrored into Linux's
tables on page faults). That cannot support user address spaces, and its physmap aliased the kernel image. The arm64
project this design follows (xnulinux) moved to real physical addresses for the same reason.

## Decision

XNU's RAM is physical memory that Linux never manages: ranges reserved with `memmap=SIZE$BASE` on the Linux command
line and claimed by `beringia_host.ko`. One range is below 4 GiB, because the kernel and XNU's bootstrap allocations
need `physical = virtual - KERNEL_BASE` (slid, like booter KASLR). Other ranges may be anywhere. XNU's pmap manages
them natively.

## Consequences

- XNU's page tables are real; CR3 can point at them.
- The reservation is fixed when Linux boots (changing it needs a WSL restart). CMA was considered: it allows resizing,
  but a multi-GiB contiguous allocation from a running system is unreliable, and the low-4-GiB constraint is easier to
  meet with a fixed reservation.
- The loader relocates the kernel (its local relocations) to the slide of the low range.
