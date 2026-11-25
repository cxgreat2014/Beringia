<!-- SPDX-License-Identifier: GPL-2.0-only -->
# 0002 — XNU owns the CPU state while it runs

Status: accepted

## Context

x86_64 has no banked kernel/user translation registers, and one GDT, IDT and TSS per CPU. XNU's native trap, syscall
and context-switch code assumes its own selectors (`KERNEL64_CS` = 0x08, `USER64_CS` = 0x2b), its own IDT and TSS (IST
stacks, RSP0) and GS-based per-CPU data. Linux's GDT gives 0x08 to a 32-bit code segment and 0x2b to its user data
segment.

Alternatives:

- (a) Run XNU on Linux's tables and rewrite XNU's entry code, selectors and per-CPU access. That is a large, deep fork
  of XNU's machine layer.
- (b) Switch the whole CPU state at each transition.

## Decision

(b). Every transition between the worlds switches CR3, GDTR/TR, IDTR, CS/SS, the FS/GS bases, the syscall MSRs,
TSC_AUX, S_CET, XCR0 and the XSAVE state. Transitions happen only at entry, host calls and forwarded interrupts.

## Consequences

- XNU's machine layer runs essentially unmodified, including user space (`sysret`/`iretq` with its own selectors).
- A transition costs a few microseconds (descriptor-table loads, MSR writes, XSAVE). Transitions are bounded by
  interrupts, idle and I/O, not by XNU's normal work.
- The world-switch code is critical to both security and stability, and gets a standalone self-test (M1).
- Reloading TR needs the TSS busy bit cleared in the target GDT. Linux's GDT is read-only (fixmap), so it is written
  through its per-CPU writable alias.
