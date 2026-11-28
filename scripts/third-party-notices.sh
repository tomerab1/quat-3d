#!/usr/bin/env bash
# Write the THIRD_PARTY_NOTICES.md shipped in a release archive: the
# repository's THIRD_PARTY_NOTICES.md (the summary table) followed by the full
# licence text of every component, taken from the build:
#   - every vcpkg port installed for the build: <build>/vcpkg_installed/*/share/<port>/copyright,
#     plus <port>/LICENSES/* where a port installs them (KTX-Software);
#   - the FetchContent packages (ACL, RTM): <build>/_deps/<name>-src/LICENSE,
#     or the FETCHCONTENT_SOURCE_DIR_<NAME> an offline build points at;
#   - the fonts in assets/fonts/ and the XeGTAO port (texts in the repository);
#   - extra files given on the command line (platform runtimes).
# Used by scripts/package-release.sh.
#
#   scripts/third-party-notices.sh <build-dir> <out-file> [<title>=<licence-file> ...]
#
# Fails when the build has no vcpkg licence files, so a wrong build directory
# cannot ship an archive without them.
set -euo pipefail

if [ $# -lt 2 ]; then
  echo "usage: $0 <build-dir> <out-file> [<title>=<licence-file> ...]" >&2
  exit 2
fi
build_dir=$1
out=$2
shift 2
root="$(cd "$(dirname "$0")/.." && pwd)"

# emit <title> <file>...: one section with each file's text in a fence.
emit() {
  local title=$1
  shift
  printf '\n#### %s\n' "$title"
  local f
  for f in "$@"; do
    # shellcheck disable=SC2016 # literal Markdown backticks
    printf '\n`%s`\n\n````text\n' "$(basename "$f")"
    # Normalise CRLF so the fences stay balanced; make sure the text ends in a newline.
    tr -d '\r' < "$f"
    [ -z "$(tail -c1 "$f")" ] || printf '\n'
    printf '````\n'
  done
}

# Ports installed for the target and host triplets, once each by name. vcpkg's
# own helper ports (vcpkg-cmake, ...) are build tools and gtest is tests only:
# neither ships.
ports=$(find "$build_dir/vcpkg_installed" -mindepth 4 -maxdepth 4 -path '*/share/*/copyright' 2>/dev/null |
        awk -F/ '{ print $(NF-1) }' | grep -v -E '^(vcpkg-.*|gtest)$' | sort -u || true)
if [ -z "$ports" ]; then
  echo "no vcpkg licence files under $build_dir/vcpkg_installed/*/share/ (not a vcpkg build directory?)" >&2
  exit 1
fi

# (sed -n 1p rather than head: head exits early, and pipefail would fail the
# pipeline on the writer's SIGPIPE.)
# fetch_source <name>: the FetchContent source tree of <name>, or nothing.
fetch_source() {
  local upper override
  upper=$(printf '%s' "$1" | tr '[:lower:]' '[:upper:]')
  override=$(sed -n "s/^FETCHCONTENT_SOURCE_DIR_${upper}:[A-Z]*=//p" "$build_dir/CMakeCache.txt" 2>/dev/null | sed -n 1p)
  if [ -n "$override" ] && [ -d "$override" ]; then
    printf '%s' "$override"
  elif [ -d "$build_dir/_deps/$1-src" ]; then
    printf '%s' "$build_dir/_deps/$1-src"
  fi
}

# A failed run leaves no half-written notices behind.
cleanup() {
  local status=$?
  [ "$status" -eq 0 ] || rm -f "$out"
}
trap cleanup EXIT

{
  tr -d '\r' < "$root/THIRD_PARTY_NOTICES.md"
  printf '\n## Licence texts\n'
  printf '\nThe licence of each component above, as installed by this build.\n'

  printf '\n### vcpkg ports\n'
  for port in $ports; do
    dir=$(find "$build_dir/vcpkg_installed" -mindepth 3 -maxdepth 3 -path "*/share/$port" | sort | sed -n 1p)
    files=("$dir/copyright")
    if [ -d "$dir/LICENSES" ]; then
      while IFS= read -r f; do files+=("$f"); done < <(find "$dir/LICENSES" -type f | sort)
    fi
    emit "$port" "${files[@]}"
  done

  printf '\n### FetchContent packages\n'
  for name in acl rtm; do
    src=$(fetch_source "$name")
    lic="${src:+$src/LICENSE}"
    if [ -z "$lic" ] || [ ! -f "$lic" ]; then
      echo "no licence file for FetchContent package $name (looked in $build_dir/_deps/$name-src and FETCHCONTENT_SOURCE_DIR_*)" >&2
      exit 1
    fi
    emit "$name" "$lic"
  done

  printf '\n### Ported code\n'
  emit "xegtao" "$root/packaging/licenses/XeGTAO-LICENSE.txt"

  printf '\n### Fonts and icons\n'
  for dir in "$root"/assets/fonts/*/; do
    name=$(basename "$dir")
    files=()
    while IFS= read -r f; do files+=("$f"); done < <(find "$dir" -maxdepth 1 -type f \( -name 'LICENSE*' -o -name 'OFL*' \) | sort)
    [ ${#files[@]} -gt 0 ] || { echo "no licence file in assets/fonts/$name" >&2; exit 1; }
    emit "$name" "${files[@]}"
  done

  if [ $# -gt 0 ]; then
    printf '\n### Platform runtimes\n'
    for arg in "$@"; do
      title=${arg%%=*}
      file=${arg#*=}
      [ -f "$file" ] || { echo "no such licence file: $file" >&2; exit 1; }
      emit "$title" "$file"
    done
  fi
} > "$out"
