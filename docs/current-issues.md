<!-- SPDX-License-Identifier: GPL-2.0-only -->
# Current implementation issues and open work

An honest account of where Beringia stands, what is broken, and what is next. Every "reaches X" below names the run
directory (`$BERINGIA_WIN_DIR/runs/<id>/`, see [debugging.md](debugging.md)) that shows it. For the architecture, see
[architecture.md](architecture.md); for milestones, [../ROADMAP.md](../ROADMAP.md).

## Where XNU gets to today (2026-11-26, run 20261126-162031)

The bare kernel (`xnu-12377.1.9` + `xnu/series`, no kernel collection) boots on one hosted CPU through:

- `vstart`, `i386_init`, pmap/VM bootstrap, zones, the scheduler, `kernel_bootstrap_thread`;
- `PE_init_iokit` (BeringiaPlatform), IOKit system capability changes, IONVRAM (stub);
- `vm_commpage_init`, `mac_policy_initmach`, `dtrace_early_init`, `trust_cache_init` (stubbed, see below),
  `machine_lockdown`, `PE_lockdown_iokit`;
- `bsd_init`: MAC framework, buffer headers, `IOKitInitializeTime` (host wall clock), lockdown mode, `mbinit`, aio;

and then stops, as expected, at:

```
panic: pthread kernel extension not loaded (function table is NULL). @pthread_shims.c:319
```

`pthread.kext` (like AppleImage4, AMFI, corecrypto, Sandbox, APFS) comes from the macOS 26 kernel collection: this is
the start of **M4**. The panic ends the run cleanly (the host unloads, Linux is unaffected).

Along the way the hosted CPU's timer, idle and interrupt forwarding work: the host timer fires and is delivered to
`rtclock_intr`, external interrupts are forwarded to Linux, `machine_idle` sleeps on the host and returns. Across the
day's runs Linux logged no warnings and the state safety net found no changes to Linux's CPU state.

## Fixed today (and why the VM used to wedge)

| Problem | Root cause | Fix |
|---|---|---|
| The whole WSL VM wedged the first time XNU idled | The hosted LAPIC uses the xAPIC ops on a RAM page, so `is_x2apic == FALSE`. `lapic_configure()` → `lapic_reinit()` read the *real* `IA32_APIC_BASE`, saw the host's x2APIC mode and called `legacy_init()`, which disables the real local APIC and re-enables it in xAPIC mode. That CPU never received another interrupt; Linux stalled waiting for its IPIs. | XNU patch "never reprogram the host's local APIC" |
| XNU writes CPU state the world switch does not switch | `pat_init`/MTRRs (with CR0.CD + `wbinvd`), the AP trampoline at physical 0x2000 (host RAM), MCA banks, TSX/SRBDS MSRs, PMU fixed counters | XNU patch "leave the host's memory types, MCA banks, errata MSRs and PMU alone"; PCIDs disabled in code |
| A timer delivered by a forwarded interrupt was missed | `interrupt()` read the pending word before forwarding | read after forwarding, atomically |
| `kernel_bootstrap_thread` blocked forever in `ml_wait_max_cpus` | nobody called `ml_set_max_cpus` (normally the ACPI CPU interrupt controller) | BeringiaPlatform announces the host's CPU count |
| 2 × 30 s waits in `IOKitInitializeTime` | no IORTC, no IONVRAM | BeringiaPlatform publishes both; time of day from the host |
| `trust_cache_init` panic | no AppleImage4/AMFI in a bare kernel | bring-up stub (no trust caches) until the KC brings AMFI |
| idle wait of 4 s instead of 4 ms | unit error in `bh_idle` | fixed |
| boot-args with spaces lost | the kernel splits module parameters at spaces | `boot.sh` passes `boot_args="..."` |

## Safety nets now in place

- **State safety net** (`beringia_host.ko statecheck=1`, default): after every exit from the foreign world, CR4, CR8,
  DR7, IA32_PAT, EFER, IA32_APIC_BASE, DEBUGCTL, MTRR_DEF_TYPE, MISC_ENABLE and the PMU controls are compared with
  their values at entry; differences are logged once and restored. A changed IA32_APIC_BASE cannot be repaired and
  ends the run.
- **Self-test** covers sleeping inside a host call (`selftest_sleep`) and the safety net (`selftest_tamper`).
- **Thread dump** (`echo 1 > /sys/module/beringia_xnu/parameters/dump`) and `tools/debug/symbolize.py`.
- **Run directories, host-side console log and the watchdog**: [debugging.md](debugging.md).

## Known limitations and deliberate bring-up choices

| Area | State |
|---|---|
| kernel collection | Not built yet. Only BeringiaPlatform is linked in statically. Next: M4. |
| trust caches | Disabled while AMFI is absent (bring-up stub, to be removed with the KC). |
| kalloc_type | Degraded to size-based data zones (upstream LLVM has no `__builtin_xnu_type_summary`). Compatibility with Apple's kexts, which carry their own kalloc_type views, is to be verified first thing in M4. |
| LAPIC | A RAM page with no-op init; timer and IPIs go through the host. |
| NMI / #MC | **Not forwarded yet**: in the foreign world they enter XNU's own handlers. |
| NVRAM | IODTNVRAM is a stub; IONVRAM is announced as ready. |
| SMP | One hosted CPU. The pending word is atomic on both sides already. |
| Physical memory | XNU's physmap maps all of RAM, the host's included, writable. Restricting it to the carve-outs is open. |
| Linux latency | While XNU runs with interrupts off, that Linux CPU takes no interrupts. Measure before SMP. |

## Next steps, in order

1. **M4, kernel collection**: extract `pthread`, `AppleImage4`, `AppleMobileFileIntegrity`, `corecrypto`, `Sandbox`,
   `IOStorageFamily`, `APFS` (and their dependencies) from the 25A353 BootKC; verify that they link and run against
   the DEVELOPMENT kernel with degraded kalloc_type; build a KC with BeringiaPlatform; remove the trust cache stub.
2. NMI / #MC forwarding; a long idle stability run (minutes, no Linux warnings).
3. **M4 exit**: "waiting for root device"; then BeringiaDisk and the SURamDisk (M5: `launchd`).
