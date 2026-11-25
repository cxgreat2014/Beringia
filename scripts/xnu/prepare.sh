#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Fetch XNU at the pinned tag and apply xnu/series on a fresh branch.
#   scripts/xnu/prepare.sh           clone (if needed), reset to the tag, apply every patch in xnu/series
#   scripts/xnu/prepare.sh --check   only verify that the series applies cleanly
set -euo pipefail
. "$(dirname "$0")/../lib/env.sh"

src=$BERINGIA_XNU_SRC
if [ ! -e "$src/.git" ]; then
	log "cloning $XNU_GIT at $XNU_TAG"
	mkdir -p "$(dirname "$src")"
	git clone -q --branch "$XNU_TAG" --depth 1 "$XNU_GIT" "$src"
fi
cd "$src"
[ "$(git rev-parse "$XNU_TAG^{commit}" 2>/dev/null || git rev-parse HEAD)" = "$XNU_COMMIT" ] ||
	echo "warning: $XNU_TAG is not $XNU_COMMIT here" >&2

if [ "${1:-}" = --check ]; then
	tmp=$(mktemp -d)
	git worktree add -q --detach "$tmp" "$XNU_COMMIT"
	trap 'git worktree remove --force "$tmp"' EXIT
	grep -v '^#' "$BERINGIA_ROOT/xnu/series" | while read -r p; do
		[ -n "$p" ] && git -C "$tmp" apply --check "$BERINGIA_ROOT/xnu/patches/$p" && echo "ok  $p"
	done
	exit 0
fi

if [ -n "$(git status --porcelain)" ]; then
	die "$src has local changes; commit or discard them first (patches are regenerated from this tree with scripts/xnu/export.sh)"
fi
git checkout -q -B beringia "$XNU_COMMIT"
grep -v '^#' "$BERINGIA_ROOT/xnu/series" | while read -r p; do
	[ -n "$p" ] || continue
	git -c user.name=Beringia -c user.email=beringia@invalid am -q --keep-cr "$BERINGIA_ROOT/xnu/patches/$p" ||
		die "patch $p does not apply"
	echo "applied $p"
done
# The ABI header shared with the loader module.
mkdir -p EXTERNAL_HEADERS/beringia
cp "$BERINGIA_ROOT"/xnu/include/beringia/*.h EXTERNAL_HEADERS/beringia/
log "XNU source ready at $src (branch beringia)"
