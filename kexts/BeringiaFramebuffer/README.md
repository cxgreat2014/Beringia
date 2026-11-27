# BeringiaFramebuffer

An `IOFramebuffer` for the boot framebuffer the Beringia loader reserves (`beringia_xnu.ko fb=WxH`, default 1280x800,
XRGB8888). It matches the device-tree node `/display` by its `compatible` (`beringia,framebuffer`, probe score 30000,
above IONDRVSupport's `IONDRVFramebuffer`, which takes the node when this kext is absent) and takes the memory from the
node's `reg` range and the geometry from the boot console (`PE_Video`).

What it adds over IONDRVSupport's `IOBootNDRV`, and why:

- **One fully described mode**: 60 Hz, a CVT reduced-blanking detailed timing, and an EDID (vendor `BRN`, product 1,
  the timing as its preferred mode), so CoreDisplay can build a mode list and identify the display.
- **It opens itself at boot** (two seconds after `start`, from a thread call), so the display connection and
  AppleDisplay exist before WindowServer looks: CoreDisplay calls a connection without an `IODisplay` unusable,
  builds no mode list (`Setting mode 0x0`) and composites into a virtual framebuffer.
- **The connect-change interrupt** (`'dci '`), raised once when the display attaches, so WindowServer rebuilds the
  connection if it looked earlier anyway.

No cursor, gamma, CLUT or vertical-blank hardware: the cursor is IOFramebuffer's software cursor, gamma and CLUT calls
succeed and are ignored. The first calls of each entry point are logged (`BeringiaFramebuffer: ...`).

State (2026-11-28): CoreDisplay sets mode 1 (`Mode has dimensions 1280x800`) and WindowServer orders windows front, but
nothing but the cursor reaches the screen yet ([../../docs/current-issues.md](../../docs/current-issues.md)).

```sh
kexts/BeringiaFramebuffer/build.sh      # $BERINGIA_BUILD/kexts/BeringiaFramebuffer/BeringiaFramebuffer.kext
```

Like the other Beringia kexts it defines a `kmod_info` (`KMOD_EXPLICIT_DECL`): OpenCore's kext linker (`tools/kc`)
refuses a kext without one (`InternalPrelinkKext` returns `EFI_UNSUPPORTED`, reported as `FAIL ... (8000000000000002)`).

## The virtual PCI device (experiment: `beringia_pci=1`)

GPUWrangler lists a display GPU only with an `IOPCIDevice` ancestor, and IOPresentment's device map needs a PCI
display-class device: a bare `IOFramebuffer` on the device-tree node is a "no GPU" (`(NG) ... pci 0 ... accel 0`) and
never receives pixels. `BeringiaPCI.cpp` therefore publishes a virtual `IOPCIDevice` (106b:eeee, class 0x030000, config
space synthesized by `configRead*`, properties published in `start`) under the node, and starts a second
`BeringiaFramebuffer` under it whose VRAM comes from `PE_Video`.

It is not on by default (`beringia_pci=1` in `BERINGIA_BOOT_ARGS` enables it), because IOPCIFamily's own
`IOPCIConfigurator` manages every published `IOPCIDevice` and expects one that a real host bridge created. The runs so
far, each an experiment that is now understood:

| Run | Result |
|---|---|
| `20261128-062114` | `IOPCIDevice::attach()` faults without an `IOPCIBridge` provider |
| `20261128-062407` | with `IOService::attach()`, `AppleGPUWrangler` faults reading the device's bridge pointer |
| `20261128-064951` | a `BeringiaPCIBridge` (`IOPCIBridge`, ten overrides) fixes that, but 106b:eeee matches AppleParavirtGPU, whose `getResources()` faults on a device without real BARs |
| `20261128-065233` | with 106b:0be1 (unmatched), `IOPCIDevice::getResources()` still faults at `+0x556` (bridge's `reserved` at `+0x170` is NULL), called from the configurator path |

The device's config space and properties are correct (`config eeee106b class 03000000`, 4-byte OSData properties). What
is missing is what a host bridge sets up before a device is published — the resource/BAR state `getResources()` walks.
The next step is a decision between an ACPI host bridge the loader describes (so Apple's own `AppleACPIPCI` enumerates
the device and owns that state) and a further look at what the configurator needs from a synthetic one.
