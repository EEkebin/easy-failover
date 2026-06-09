#!/usr/bin/env bash
# Print the package version to stdout.
#
#   - Tagged release (arg is a refs/tags/vX.Y.Z ref, or a vX.Y.Z string):
#       the tag with the leading "v" stripped -> X.Y.Z
#   - Otherwise (rolling build from main / dev): calendar version
#       YYYY.MM.DD.<commit-count>, e.g. 2026.06.08.342
#
# The commit count (git rev-list --count HEAD) is strictly monotonic, so the
# version always increases across commits and `apt`/`dnf` upgrade to the newest
# rolling build correctly. The short commit hash is for humans (release notes),
# not the orderable version, because a hex hash does not sort monotonically.

set -euo pipefail

ref="${1:-}"

case "${ref}" in
    refs/tags/v*)
        printf '%s\n' "${ref#refs/tags/v}"
        exit 0
        ;;
    v[0-9]*)
        printf '%s\n' "${ref#v}"
        exit 0
        ;;
esac

date_part="$(date -u +%Y.%m.%d)"
count="$(git rev-list --count HEAD 2>/dev/null || echo 0)"
printf '%s.%s\n' "${date_part}" "${count}"
