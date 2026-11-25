<!-- SPDX-License-Identifier: GPL-2.0-only -->
# macOS 26.0 (25A353) assets

Beringia validates against macOS 26.0 build 25A353 for x86_64. None of these files are in the repository. Obtain them
from your own licensed copy of macOS, or from Apple's public software-update servers, and keep them under
`BERINGIA_ASSETS` (default `~/beringia-work/assets/25A353`).

| File | Used for | How to check it |
|---|---|---|
| `BootKernelExtensions.kc` | Apple's kexts for the hosted kernel collection (M4) | Mach-O x86_64, `MH_FILESET`; embeds `xnu-12377.1.9~3/RELEASE_X86_64` |
| `BaseSystem.dmg.trustcache` (`bstc`), `x86_64SURamDisk.dmg.trustcache` (`rtsc`) | Code-signing trust caches (M5) | IM4P containers |
| `x86_64SURamDisk.dmg` | First userspace root (M5) | APFS image, 4096-byte blocks |
| Full OTA payload (`payload.000` …) | The complete system volume (M6) | pbzx chunks of a `YAA1` Apple Archive |
| `SystemVersion.plist` | Version check | `ProductVersion 26.0`, `ProductBuildVersion 25A353` |
| Kernel Debug Kit 25A353 | Headers and libraries for the build | `kernel.development` is `xnu_development-12377.1.9~2` |

`scripts/assets/` fetches individual members of the OTA ZIP with HTTP range requests and verifies sizes and CRCs
against the ZIP's central directory; `scripts/assets/decode-pbzx.py` turns payload chunks into the Apple Archive.
Extract the archive on a POSIX file system (ext4 in WSL): Windows file systems lose modes and symlinks.

Note: the Intel "update" payload is a delta subset (no `launchd`, no `dyld`). The complete system comes from the
full OTA.
