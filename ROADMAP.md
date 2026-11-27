# Roadmap

Beringia is built in public. Each milestone has an **exit criterion that can be checked** (a log line, a test, a
screenshot), and nothing is marked done without it. Status legend: ✅ done · 🔨 in progress · ⏳ next · ⬜ not started.

| | Milestone | Exit criterion | Status |
|---|---|---|---|
| M0 | Reproducible XNU build on Linux | `scripts/xnu/build.sh` produces `kernel.development` (x86_64, `xnu-12377.1.9`) from a pristine tag + `xnu/series` | ✅ |
| M1 | Host foundation | Carve-outs claimed; world-switch self-test: enter a test world (own CR3/GDT/IDT/TSS), take and forward a Linux timer interrupt, return, 10⁶ round trips without a Linux warning | ✅ |
| M2 | XNU early boot | `vstart` → `i386_init` → `kernel_bootstrap_thread`; XNU's console and panics reach the host log | ✅ |
| M3 | Time, idle, CPUs | Scheduler runs with a host timer; idle sleeps in Linux; 1 then N hosted CPUs with IPIs; no RCU stall / soft lockup over 10 min | 🔨 (single CPU: host timer, idle and interrupt forwarding work; 10-min run and SMP not done) |
| M4 | Platform & kernel collection | `BeringiaPlatform`/`BeringiaCPU` kexts; KC built on Linux from Beringia's kernel + 25A353 Apple kexts; IOKit reaches "waiting for root device" | 🔨 (boot path done: legacy prelinked kernel + security stack, 14-min clean root-device wait; standalone BeringiaCPU not done) |
| M5 | First macOS 26 userspace | `BeringiaDisk` + 25A353 recovery ramdisk as root; `launchd` (PID 1) starts and runs a shell | 🔨 (the full system's launchd and ~130 daemons run from BeringiaDisk; no console input, so no shell yet) |
| M6 | Full macOS 26 system | System volume built from the 25A353 OTA payload; multi-user boot; console login; network (`BeringiaNet`), `ssh` into macOS | ⬜ |
| M7 | Display & input | Framebuffer + input kexts; WindowServer and loginwindow on the CPU renderer; screenshot from the host | 🔨 (boot framebuffer + `BeringiaFramebuffer`; WindowServer runs, CoreDisplay accepts the mode and draws its cursor, screenshots through `/dev/beringia-fb`; the software compositor does not present yet, no input) |
| M8 | Productization | One `beringia` CLI (build, disk, boot, console); CI for modules + XNU; test suite; docs for non-WSL hosts | ⬜ |

## Now

- 2026-11-28: WindowServer runs and draws. Its own unified log (now readable on Linux) said `IOHIDSystem not found`;
  user-space backtraces in the thread dump then showed each next wait (GPU wrangler, IOSurface, Bluetooth, FSKit's
  LiveFS) and the loader gained a boot framebuffer that `BeringiaFramebuffer` drives (an IOFramebuffer with one fully
  described mode and an EDID; IONDRVSupport's own framebuffer leaves the display offline). The image had lost 23 427
  hard-linked names in extraction, powerd's launchd plist among them; `tools/disk/mtree.py` restores them from the
  OTA's mtree. The first-boot Language Chooser runs and waits for input; its window is ordered front but only the
  cursor reaches the screen, because the software compositor does not present to the framebuffer yet
  ([docs/research/m5-userspace-bringup.md](docs/research/m5-userspace-bringup.md), section 7).
- 2026-11-27 (audit): the full 25A353 system boots from a 40 GiB APFS image on BeringiaDisk past launchd's early
  boot; kernelmanagerd checks in and ~130 Apple daemons run without a kernel panic. That took the CSR flag, the
  platform UUID from `/efi/platform/system-id`, BeringiaSMC for DSMOS, and removing a registry nub without a property
  table (patch 0045), plus IOUserSerial for a dext that kept the registry busy. WindowServer now launches and exits
  at once (`exit(1)`): there is no framebuffer or input yet, and the image still lacks most of the OS cryptex. The
  audit of the M4/M5 work and its fixes: [docs/audit/2026-11-27-m5-session-audit.md](docs/audit/2026-11-27-m5-session-audit.md).
- 2026-11-27: BeringiaDisk mounts the APFS recovery image as `disk0`. Clearing CR4.UMIP on foreign entry fixes
  launchd's `sidt` #GP, and patch 0038 fixes property-table-less registry nubs; launchd now starts ramrod and
  syslogd and shuts down only because the target System volume is absent. The current runtime gap, evidence and
  reproduction steps are in [docs/research/m5-userspace-bringup.md](docs/research/m5-userspace-bringup.md).
- M0 is done: a clean `xnu-12377.1.9` plus three build patches (`xnu/patches/build/`) links a 23.6 MB
  `kernel.development` on WSL2.
- M1 is done: `beringia_host.ko`'s self-test runs a minimal foreign kernel (own CR3, GDT, IDT with IST, TSS,
  selectors, MSRs, XSAVE state) on 6 CPUs for 30 s: 10.9 M world round trips, 10.9 M exceptions taken by the foreign
  IDT, 23 649 Linux interrupts (Hyper-V timer, VMBus, IPIs) forwarded to Linux, no kernel warning
  ([tests/results/m1-selftest.log](tests/results/m1-selftest.log), `sudo scripts/run/selftest.sh 30 6`).
- M2 is done: `beringia_xnu.ko` loads the kernel (Mach-O, slid headers, local relocations), and hosted XNU boots
  through `vstart`, `i386_init`, the whole memory subsystem (pmap, VM bootstrap, zones) and the scheduler into
  **IOKit platform matching** (`PE_init_iokit` → phase 17). Console output and full panic backtraces reach the host
  log (`dmesg` and the synchronous `/mnt/c` trace). Hosted patches cover: the shared kernel half, TSC frequency from
  the host, a fake LAPIC page, interrupt forwarding, host timer + idle, a host CSPRNG fallback for early boot, and
  ending the run on panic instead of resetting the VM.
- 2026-11-26: the whole-VM hang at XNU's first idle was XNU resetting the host's local APIC (`lapic_reinit` →
  `legacy_init`). With that and the other hosted guards fixed, the bare kernel boots through IOKit, `trust_cache_init`
  (stubbed), `machine_lockdown` and into `bsd_init`, where it stops at "pthread kernel extension not loaded": the
  kernel collection (M4) is next. Details and run directories: [docs/current-issues.md](docs/current-issues.md).
- 2026-11-26 (late): M5 begins. The loader boots a RAM-disk root (`ramdisk=`); HFS.kext and apfs.kext from the
  KDK mount filesystems; Apple's recovery ramdisk runs its real launchd (ignition banner), the first Apple userland
  on Beringia. Next: the ignition stall and a real system volume.
- 2026-11-26 (night): M4-c and the M4 exit. The security stack (28 kexts) loads: AppleImage4, AMFI, Sandbox,
  Quarantine, AppleSystemPolicy, AppleKeyStore; `trust_cache_init` passes; 14 minutes at the root-device wait with
  no Linux warnings. `tools/kc/deps.py` computes the kext closure (honouring `OSBundleLibraries_x86_64`).
- 2026-11-26 (evening): M4 spike. Apple's kexts link on Linux with OpenCorePkg's OcAppleKernelLib into a legacy
  prelinked kernel; with corecrypto (FIPS self-test passes) and pthread, XNU boots to "Still waiting for root device"
  and idles there cleanly for 10 minutes. Same C++ ABI as Apple's kernel (all 348 vtables, slot by slot).
  [docs/research/m4-spike.md](docs/research/m4-spike.md).

## Known open questions

Tracked as issues once the repository is public; listed here so they are not forgotten.

- **kalloc_type on upstream LLVM.** Apple's `__builtin_xnu_type_summary` does not exist in upstream LLVM; the build
  patch currently classifies every type as data. That changes zone segregation and may matter for zone-check panics.
  A faithful replacement (a clang plugin or a pass computing the summary) may be needed.
- **iig output.** DriverKit interface code is pre-generated for a later XNU and adapted by `tools/build/iig_fixup.py`.
- **Hyper-V specifics.** TSC frequency, x2APIC, synthetic timers and MSRs that Hyper-V refuses (#GP) on paths XNU
  takes at boot (power management, MCA, microcode).
- **Code signing.** The kernel is not Apple's; AMFI policy for 25A353 userspace (trust caches, `amfi_get_out_of_my_way`)
  needs a decision for M5.
- **Kernel collection on Linux.** Building an x86_64 `MH_FILESET` boot KC without `kmutil`.

The previous, abandoned approach and why: [docs/audit/2026-11-prior-x86-attempt.md](docs/audit/2026-11-prior-x86-attempt.md).
