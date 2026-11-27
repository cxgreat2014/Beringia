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

## The DSMOS key (`BERINGIA_OSK_FILE`)

Apple's `Dont Steal Mac OS X.kext` (DSMOS) decrypts protected system binaries with a key it reads from the SMC: the
64-character text that the SMC of every Intel Mac returns for the keys `OSK0` (first 32 characters) and `OSK1` (last
32). Beringia has no SMC; `kexts/BeringiaSMC` answers DSMOS instead, **with a key you supply**. The key is Apple's,
so it is not in this repository, in any build output, or in any log: read it from your own Mac's SMC (any SMC key
reader that can read `OSK0` and `OSK1`; QEMU's `applesmc` device asks for the same value).

Put the 64 characters, OSK0 then OSK1, in a file outside the repository and name it in `local.env`:

```sh
install -d -m 700 ~/beringia-work/secrets
printf '%s' '<OSK0><OSK1>' > ~/beringia-work/secrets/dsmos-osk.txt    # the 64 characters, nothing else
chmod 600 ~/beringia-work/secrets/dsmos-osk.txt
echo "BERINGIA_OSK_FILE=$HOME/beringia-work/secrets/dsmos-osk.txt" >> local.env
```

`boot.sh` passes the file to the loader (`beringia_xnu.ko osk=`, not visible in sysfs), which checks that it holds
64 printable characters and hands it to XNU as the device-tree property `/efi/platform/beringia-osk`; BeringiaSMC
takes it and removes the property before user space starts. Without the file the boot goes on, BeringiaSMC logs
`no DSMOS key supplied` and DSMOS keeps waiting, so the binaries it protects cannot run.
