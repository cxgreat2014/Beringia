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
