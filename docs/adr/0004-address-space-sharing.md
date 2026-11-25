<!-- SPDX-License-Identifier: GPL-2.0-only -->
# 0004 — How the two kernels share the kernel half

Status: accepted

## Context

Both kernels live in the upper canonical half:

- XNU's kernel is in PML4 slot 511, from PDPT entry 0 up. XNU also uses slot 510 (the kext basement), physmap slots
  below it, and a double-map slot.
- Linux's image, modules and fixmap are also in slot 511, at PDPT entries 510–511. Linux randomizes its direct map,
  vmalloc and vmemmap within slots 273–507, and uses slots 508 and 510.

## Decision

- XNU's page tables include Linux's slot-511 PDPT entries 510/511. These map module code and data, which the
  trampolines need.
- XNU's kernel VA ceiling is lowered below Linux's region in slot 511.
- XNU's physmap and double-map slots are chosen at boot among slots Linux does not use.
- Slot 510 is XNU's kext basement in XNU's tables and Linux's espfix in Linux's. XNU's mappings there are not global.
- The host never dereferences XNU virtual addresses. Host calls pass physical addresses, and the host reaches XNU RAM
  through its own mapping of the carve-outs.

## Consequences

- No synchronization of page tables between the kernels at run time.
- Paravirtual drivers describe buffers by physical segments, as a DMA device would.
