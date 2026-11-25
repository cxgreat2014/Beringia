# xnu/

Changes to Apple's XNU, as patches against **`xnu-12377.1.9`** (commit `f6217f891ac0bb64f3d375211650a4c1ff8ca1ea`,
the macOS 26.0 / 25A353 kernel), applied in the order of [`series`](series) by `scripts/xnu/prepare.sh`.

| Directory | What |
|---|---|
| `patches/build/` | Building XNU on a Linux host with upstream LLVM (no runtime effect) |
| `patches/hosted/` | The BERINGIA hosted machine glue: each patch one topic, all guarded by `beringia_hosted()` |
| `include/beringia/` | The ABI shared with `beringia_xnu.ko` (MIT), copied into `EXTERNAL_HEADERS/beringia/` |

Workflow:

```sh
scripts/xnu/prepare.sh          # pristine tag + series on branch "beringia" in $BERINGIA_XNU_SRC
# edit, commit in $BERINGIA_XNU_SRC (one topic per commit; subject "build: ..." for build-only changes)
scripts/xnu/export.sh           # regenerate patches/ and series from those commits
scripts/xnu/prepare.sh --check  # the series applies to the pristine tag
```

Patches are APSL-2.0 (modifications of APSL code).
