<!-- SPDX-License-Identifier: GPL-2.0-only -->
# tools/disk

Tooling for the macOS system image. Nothing here contains or downloads Apple files; it works on the assets you
obtained yourself ([../../docs/setup/macos-assets.md](../../docs/setup/macos-assets.md)).

## mtree.py: is the staged tree complete?

A full macOS OTA carries an mtree of the sealed system volume it installs (for 25A353 x86_64,
`090-87660-607.dmg.aea.x86.mtree`, next to its root hash and trust cache; an IM4P whose payload is PBZE/LZFSE-compressed
and holds a small Apple Archive with `mtree.txt` and `digest.db`). The mtree lists every path with its type and APFS inode number, which makes it the reference for what an
extracted tree must contain.

```sh
python3 -I tools/disk/mtree.py list  mtree.txt | head            # type, inode, path
sudo python3 -I tools/disk/mtree.py check mtree.txt ROOT          # what ROOT is missing, by type and directory
sudo python3 -I tools/disk/mtree.py check mtree.txt ROOT --fix-hardlinks --json report.json
```

Apple Archive stores a hard-linked file once. An extractor that does not recreate the other names loses them, and
some of those names matter: `/System/Library/LaunchDaemons/com.apple.powerd.plist` is a hard link to
`powerd.bundle/com.apple.powerd.plist`, so launchd never started powerd, and `timed`, `coreaudiod` and `apsd` failed
on `IOPMConnectionCreate`. For the 25A353 tree used in M5, 23 427 names in 9 476 hard-link groups were missing (launchd
plists, `CodeResources`, `version.plist`, framework resources), and nothing else. `--fix-hardlinks` creates each
missing name with link(2) from a name of the same inode that is present; ROOT can be an ext4 staging tree or the image
itself mounted read-write with linux-apfs-rw.

Decoding the IM4P needs an LZFSE decoder (Apple's open-source lzfse, BSD-3-Clause); extracting `mtree.txt` from the
archive is a few lines over the AA01 format. Both belong in the image builder that is still to be written (M6).

## overrides.sh, launchd-disabled.py: bring-up changes to the image

`sudo tools/disk/overrides.sh [--undo] [IMAGE]` makes the changes a machine without Bluetooth hardware needs (its
header says what and why); it is idempotent and `--undo` reverts it. The golden image is `chattr +i`: lift that for
the change and put it back after.

`tools/disk/launchd-disabled.py [--undo] LABEL... ROOT` disables launchd jobs through launchd's own override file
(`/private/var/db/com.apple.xpc.launchd/disabled.plist`) on a mounted volume, without touching the jobs' plists. With
the mount point last it fits `scripts/run/experiment.sh --image-patch`, which tries a change on one run's copy only:

```sh
sudo scripts/run/experiment.sh --note "without Installer Progress" \
  --image-patch "python3 -I tools/disk/launchd-disabled.py com.apple.InstallerProgress"
```
