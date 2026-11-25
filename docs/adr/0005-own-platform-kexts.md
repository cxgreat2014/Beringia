<!-- SPDX-License-Identifier: GPL-2.0-only -->
# 0005 — Own platform kexts instead of AppleACPIPlatform

Status: accepted

## Context

On Intel Macs, the platform expert, interrupt routing and timers come from AppleACPIPlatform and related kexts, which
drive real ACPI, HPET, IOAPIC, PCI and the SMC. Under WSL2 none of that hardware belongs to XNU, and most of it does not
exist. A previous attempt linked AppleACPIPlatform into the kernel and presented the machine as an iMac20,1.

## Decision

Beringia provides its own `BeringiaPlatform` (IOPlatformExpert) and `BeringiaCPU` (IOCPU) kexts, plus paravirtual
device kexts. Apple kexts are used only where they do not depend on hardware: corecrypto, AMFI, Sandbox, APFS, the
storage and network families, and so on.

## Consequences

- Some userspace components may expect Intel platform services (SMC keys, ACPI power states). Stand-ins are added only
  when a specific component needs one.
