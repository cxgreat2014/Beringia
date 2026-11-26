#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Regenerate xnu/patches from the commits on top of the pinned tag in $BERINGIA_XNU_SRC.
# Each commit is one patch (unnumbered subjects, so adding a commit does not rewrite every file); a commit subject "build: ..." goes to patches/build/, everything else to patches/hosted/.
set -euo pipefail
. "$(dirname "$0")/../lib/env.sh"

cd "$BERINGIA_XNU_SRC"
[ -z "$(git status --porcelain)" ] || die "uncommitted changes in $BERINGIA_XNU_SRC"
out=$BERINGIA_ROOT/xnu/patches
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
git format-patch -q -N --no-signature --zero-commit -o "$tmp" "$XNU_COMMIT..HEAD"
rm -f "$out"/build/*.patch "$out"/hosted/*.patch
mkdir -p "$out/build" "$out/hosted"
series=$BERINGIA_ROOT/xnu/series
echo "# Applied in order onto apple-oss-distributions/xnu tag $XNU_TAG (scripts/xnu/prepare.sh)." > "$series"
nb=0 nh=0
for p in "$tmp"/*.patch; do
	# the whole Subject header, unfolded: the file name must not depend on where git wrapped it
	subj=$(awk '/^Subject: /{s=$0; while ((getline l) > 0 && l ~ /^[ \t]/) s=s l; print s; exit}' "$p" |
		sed 's/^Subject: \[PATCH[^]]*\] //; s/[[:space:]]\+/ /g')
	slug=$(echo "$subj" | sed 's/^[a-z]*: //; s/[^A-Za-z0-9]\+/-/g; s/^-//; s/-$//' | tr 'A-Z' 'a-z' | cut -c1-48)
	case "$subj" in
	build:*) nb=$((nb + 1)); dir=build; n=$nb ;;
	*) nh=$((nh + 1)); dir=hosted; n=$nh ;;
	esac
	name=$(printf '%s/%04d-%s.patch' "$dir" "$n" "$slug")
	cp "$p" "$out/$name"
	echo "$name" >> "$series"
done
log "exported $nb build and $nh hosted patches"
cat "$series"
