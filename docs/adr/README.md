<!-- SPDX-License-Identifier: GPL-2.0-only -->
# Architecture decision records

One file per decision that had a real alternative. Format: context, decision, consequences. Status is one of
*accepted*, *superseded by NNNN*, *proposed*.

| ADR | Decision | Status |
|---|---|---|
| [0001](0001-real-physical-memory.md) | XNU manages real, reserved physical memory | accepted |
| [0002](0002-full-world-switch.md) | XNU owns the CPU state (CR3, GDT, IDT, TSS, MSRs) while it runs | accepted |
| [0003](0003-interrupt-forwarding.md) | Hardware interrupts are forwarded to Linux with `int $vector` | accepted |
| [0004](0004-address-space-sharing.md) | Kernel-half sharing: PDPT-level in slot 511, physical addresses across the ABI | accepted |
| [0005](0005-own-platform-kexts.md) | Own platform kexts instead of AppleACPIPlatform | accepted |
| [0006](0006-target-macos-26-25A353.md) | Target macOS 26.0 (25A353) and xnu-12377.1.9 | accepted |
