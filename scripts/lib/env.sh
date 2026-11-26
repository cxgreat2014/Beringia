# SPDX-License-Identifier: GPL-2.0-only
# shellcheck shell=bash
# Common environment for Beringia's scripts. Source it; do not run it.
#
# Everything Beringia downloads or builds lives under BERINGIA_WORK (default ~/beringia-work), never in the
# repository. Any path below can be overridden from the environment or from a local file, $BERINGIA_ROOT/local.env
# (git-ignored), e.g. to reuse a toolchain that is already built elsewhere.

BERINGIA_ROOT=${BERINGIA_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}
[ -f "$BERINGIA_ROOT/local.env" ] && . "$BERINGIA_ROOT/local.env"

: "${BERINGIA_WORK:=$HOME/beringia-work}"
: "${BERINGIA_JOBS:=$(nproc)}"

# Pinned upstreams.
: "${XNU_GIT:=https://github.com/apple-oss-distributions/xnu.git}"
: "${XNU_TAG:=xnu-12377.1.9}"                                  # macOS 26.0, build 25A353
: "${XNU_COMMIT:=f6217f891ac0bb64f3d375211650a4c1ff8ca1ea}"
: "${XNU_PROJECT_VERSION:=12377.1.9}"
: "${XNU_DARWIN_VERSION:=25.0.0}"
: "${MACOS_BUILD:=25A353}"
: "${DROIDVM_GIT:=https://github.com/Droid-VM/macOS-DroidVM}"  # Linux-hosted Darwin toolchain (cctools, MIG, tool shims)
: "${DROIDVM_COMMIT:=524d27b}"
: "${LLVM_BIN:=/usr/lib/llvm-21/bin}"
: "${OPENCORE_GIT:=https://github.com/acidanthera/OpenCorePkg.git}"  # OcAppleKernelLib, the kext linker (tools/kc)
: "${OPENCORE_COMMIT:=ebea824880706ac9faa8ee45604245de62be658b}"
: "${AUDK_GIT:=https://github.com/acidanthera/audk.git}"              # the EDK2 headers OpenCorePkg builds against
: "${AUDK_COMMIT:=dfd5516702b4911f1fc1919a429d637b456df913}"

# Layout under BERINGIA_WORK.
: "${BERINGIA_DROIDVM:=$BERINGIA_WORK/deps/macOS-DroidVM}"
: "${BERINGIA_SDK:=$BERINGIA_DROIDVM/toolchains/sdks/MacOSX26.1.sdk}"
: "${BERINGIA_TOOLCHAIN:=$BERINGIA_WORK/toolchain/x86_64}"      # cctools/MIG for x86_64-apple-darwin, tool shims
: "${BERINGIA_KDK:=$BERINGIA_WORK/kdk/$MACOS_BUILD}"           # headers and libraries from Apple's KDK (not shipped here)
: "${BERINGIA_KDK_EXTENSIONS:=$BERINGIA_KDK/System/Library/Extensions}"  # its kexts (M4)
: "${BERINGIA_OPENCORE:=$BERINGIA_WORK/deps/OpenCorePkg}"           # tools/kc dependencies
: "${BERINGIA_AUDK:=$BERINGIA_WORK/deps/audk}"
: "${BERINGIA_FAKEROOT:=$BERINGIA_WORK/fakeroot}"               # the header-only SDK root XNU compiles against
: "${BERINGIA_XNU_SRC:=$BERINGIA_WORK/src/xnu}"                 # pristine tag + xnu/series applied
: "${BERINGIA_BUILD:=$BERINGIA_WORK/build}"
: "${BERINGIA_ASSETS:=$BERINGIA_WORK/assets/$MACOS_BUILD}"      # your own macOS 26 assets (never committed)
: "${BERINGIA_XNU_MK:=$BERINGIA_TOOLCHAIN/xnu-linux-x86.mk}"
: "${BERINGIA_IIG:=$BERINGIA_DROIDVM/k27/iig}"                  # pre-generated iig outputs (Apple's iig is not public)

# The Linux kernel tree the host modules build against (needs Module.symvers).
: "${LINUX_KDIR:=/lib/modules/$(uname -r)/build}"

export BERINGIA_ROOT BERINGIA_WORK BERINGIA_JOBS XNU_TAG XNU_COMMIT MACOS_BUILD

die() { echo "error: $*" >&2; exit 1; }
log() { echo "==> $*"; }
