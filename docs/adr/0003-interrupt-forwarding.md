<!-- SPDX-License-Identifier: GPL-2.0-only -->
# 0003 — Hardware interrupts are forwarded to Linux

Status: accepted

## Context

While XNU runs, its IDT is loaded. Linux must still see its device, timer and IPI interrupts promptly: Hyper-V VMBus,
the synthetic timer, TLB shootdowns, RCU.

Alternatives:

- Keep hardware interrupts masked in XNU and poll at interrupt-enable points. This is the arm64 project's approach; it
  bounds Linux's interrupt latency by XNU's code paths.
- Forward each interrupt as it arrives.

## Decision

XNU runs with its normal interrupt flag. An external interrupt (or an NMI or machine check) enters XNU's IDT. The hosted
`interrupt()` makes a host call that switches to the Linux world and executes `int $vector` through Linux's IDT, so
Linux's handler runs and sends the EOI for the LAPIC vector, which is still in service. On return, XNU processes what
the host marked pending for it (timer deadline, IPIs) in that same interrupt context.

## Consequences

- Linux's interrupt latency while XNU runs is XNU's interrupt-disabled time plus one world switch.
- XNU never programs the LAPIC; its timer and IPIs are host services.
- The forwarded handler runs on the CPU thread's Linux stack, with Linux's CR3 and GS. From Linux's point of view it
  is an ordinary interrupt from kernel mode, with preemption disabled.
