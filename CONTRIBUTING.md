# Contributing

Beringia is developed in the open; issues and pull requests are welcome.

## Ground rules

- **No Apple binaries** in the repository or in pull requests (kernels, kexts, KCs, trust caches, SDK/KDK content,
  disk images, screenshots with private data are fine to discuss but not to commit as assets).
- **Respect the license line.** `host/` (GPL-2.0-only) may use Linux internals and must not include XNU code.
  `loader/`, `kexts/` and `xnu/` (APSL-2.0) must not include Linux code or headers beyond `host/include/beringia/host.h`.
- **Every file has an `SPDX-License-Identifier`** (`scripts/check-spdx.sh` runs in CI).
- **XNU changes are patches** in `xnu/patches/`, one topic per patch, generated with `git format-patch` from a tree
  prepared by `scripts/xnu/prepare.sh` (`scripts/xnu/export.sh` regenerates them). Hosted changes are guarded by
  `beringia_hosted()` so that the patched tree still builds and behaves as a stock kernel when not hosted.
- **Evidence for claims.** A milestone or a fix is "done" when its effect is observed (log excerpt, test output) and
  the evidence is linked in the pull request.
- Decisions with real alternatives get an ADR in `docs/adr/`.

## Style

- C: Linux kernel style in `host/`; XNU style (tabs, XNU's brace style) in `xnu/patches` and `loader/`.
- Shell: `bash`, `set -euo pipefail`, source `scripts/lib/env.sh`, no hard-coded user paths.
- Python: standard library only where possible, run with `python3 -I`.

## Commit messages

`area: summary` (areas: `host`, `loader`, `xnu`, `kexts`, `scripts`, `tools`, `docs`, `ci`), body explaining why.
