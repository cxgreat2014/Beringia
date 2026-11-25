#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Every source file must name its license (see LICENSE). Files without a comment syntax are exempt.
set -euo pipefail
cd "$(dirname "$0")/.."
bad=0
while IFS= read -r f; do
	case "$f" in
	LICENSES/*|LICENSE|*.patch|*.json|*.plist|series|.gitignore|.editorconfig|*/series|README.md|ROADMAP.md|NOTICE.md|CONTRIBUTING.md|*/README.md) continue ;;
	esac
	if ! head -n 5 "$f" | grep -q 'SPDX-License-Identifier:'; then
		echo "missing SPDX-License-Identifier: $f"
		bad=1
	fi
done < <(git ls-files)
exit $bad
