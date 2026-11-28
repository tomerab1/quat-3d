#!/usr/bin/env bash
# Find the curated release notes for a tag: docs/releases/<tag>.md.
# Used by .github/workflows/release.yml (an early check in the package job,
# and the publish job, which hands the file to the release as its body).
#
#   scripts/release-notes.sh <tag>
#
# Prints the notes path when the file exists and is not empty. A prerelease
# (any tag with a '-', e.g. v0.2.0-rc.1, the same rule the workflow uses) may
# have no notes: it then prints nothing and the release carries GitHub's
# generated notes only. A release tag without notes is an error.
# See docs/RELEASING.md.
set -euo pipefail

if [ $# -ne 1 ] || [ -z "$1" ]; then
  echo "usage: $0 <tag>" >&2
  exit 2
fi
tag=$1
notes="docs/releases/$tag.md"
root="$(cd "$(dirname "$0")/.." && pwd)"

if [ -s "$root/$notes" ]; then
  echo "$notes"
elif [[ "$tag" == *-* ]]; then
  echo "::notice::no $notes: prerelease $tag gets GitHub's generated notes only" >&2
else
  echo "::error::release $tag has no notes: $notes is missing or empty. Write it on master (docs/RELEASING.md), then tag the merge commit again." >&2
  exit 1
fi
