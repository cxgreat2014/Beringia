# tools/ (GPL-2.0-only)

| Directory | What |
|---|---|
| `build/` | Build helpers (`iig_fixup.py`) |
| `kc/` | Prelinked kernel builder: the kernel plus Apple kexts linked with OpenCorePkg (M4) |
| `disk/` | System image tooling: `mtree.py` checks a staged tree against the OTA's system-volume mtree and restores lost hard links ([disk/README.md](disk/README.md)); the full image builder is still to come |
| `debug/` | Run-time debugging ([../docs/debugging.md](../docs/debugging.md)): `symbolize.py` (kernel addresses), `dyldsym.py` (user addresses in the dyld shared cache), `peek2bin.py` (memory peeks to a binary), `fbshot.py` (screenshots of the framebuffer), `harvest.sh` (macOS's own logs from the image), `run-summary.py` (summary.txt/json of a run directory; `scripts/run/experiment.sh` runs all of these for one experiment) |
