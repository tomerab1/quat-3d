#!/usr/bin/env bash
# Write the THIRD_PARTY_NOTICES.md shipped in a release archive: the
# repository's THIRD_PARTY_NOTICES.md (the summary table) followed by the full
# licence text of every component, taken from the build:
#   - every vcpkg port installed for the build: <build>/vcpkg_installed/*/share/<port>/copyright,
#     plus <port>/LICENSES/* where a port installs them (KTX-Software);
#   - the FetchContent packages (ACL, RTM): <build>/_deps/<name>-src/LICENSE,
#     or the FETCHCONTENT_SOURCE_DIR_<NAME> an offline build points at;
#   - the fonts in assets/fonts/, the XeGTAO port and the vendored MikkTSpace
#     (texts in the repository);
#   - the Rust crates compiled into quat-mcp (scripts/rust-notices.py over
#     `cargo metadata`, texts from the Cargo registry: build quat-mcp first),
#     unless --no-rust (a package without quat-mcp: games from quat-pack);
#   - with --runtimes <exe>, the platform runtimes a package bundles: on
#     Linux the Debian/Ubuntu copyright file of the package owning each
#     libc++ / libunwind library <exe> loads (as lib/ ships them), on macOS
#     the licence files of Homebrew's molten-vk and vulkan-loader kegs (as the
#     .app ships them); Windows bundles only the MSVC runtime, whose terms the
#     summary names;
#   - extra files given on the command line.
# Used by scripts/package-release.sh, which also writes the --no-rust variant
# to <build-dir>/THIRD_PARTY_NOTICES.md for the game packages quat-pack lays
# out from the same build.
#
#   scripts/third-party-notices.sh [--no-rust] [--runtimes <exe>] <build-dir> <out-file>
#                                  [<title>=<licence-file> ...]
#
# Fails when the build has no vcpkg licence files, so a wrong build directory
# cannot ship an archive without them.
set -euo pipefail
# Licence texts are copied byte for byte, and some are not valid UTF-8 (Latin-1
# copyright lines). macOS's BSD tr/sed reject those under a UTF-8 locale with
# "Illegal byte sequence", so process bytes, which also gives a byte-order sort.
export LC_ALL=C

usage() {
  echo "usage: $0 [--no-rust] [--runtimes <exe>] <build-dir> <out-file> [<title>=<licence-file> ...]" >&2
  exit 2
}
with_rust=1
runtimes_of=""
while [ $# -gt 0 ]; do
  case "$1" in
    --no-rust) with_rust=0; shift ;;
    --runtimes) [ $# -ge 2 ] || usage; runtimes_of=$2; shift 2 ;;
    --*) echo "unknown option: $1" >&2; usage ;;
    *) break ;;
  esac
done
[ $# -ge 2 ] || usage
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
rust_notices="$out.rust.tmp"
cleanup() {
  local status=$?
  rm -f "$rust_notices"
  [ "$status" -eq 0 ] || rm -f "$out"
}
trap cleanup EXIT

if [ "$with_rust" = 1 ]; then
  # Python 3 under whichever name this OS has (Windows runners: python or py),
  # given paths it can open (POSIX-style shell paths are converted on Windows).
  python=""
  for candidate in python3 python py; do
    if command -v "$candidate" > /dev/null &&
       "$candidate" -c 'import sys; sys.exit(sys.version_info < (3, 9))' 2> /dev/null; then
      python=$candidate
      break
    fi
  done
  [ -n "$python" ] || { echo "no Python 3.9+ for the Rust notices (scripts/rust-notices.py)" >&2; exit 1; }
  native() { if command -v cygpath > /dev/null; then cygpath -w "$1"; else printf '%s' "$1"; fi; }
  "$python" -I "$(native "$root/scripts/rust-notices.py")" markdown \
    "$(native "$root/tools/quat-mcp/Cargo.toml")" "$(native "$rust_notices")"
else
  : > "$rust_notices"
fi

# The platform runtimes a package bundles next to the executable (see the
# header), as <title>=<file> arguments after the command line's own.
if [ -n "$runtimes_of" ]; then
  [ -f "$runtimes_of" ] || { echo "--runtimes: no executable at $runtimes_of" >&2; exit 1; }
  case "$(uname -s)" in
  Linux)
    # The libraries as the build machine resolves them (system paths), each
    # package once. A library no package owns has no licence file to ship:
    # that is an error, not a warning, since the package would bundle it.
    seen=" "
    while read -r lib; do
      [ -n "$lib" ] || continue
      deb=$(dpkg -S "$(readlink -f "$lib")" 2>/dev/null | sed -n '1s/:.*//p' || true)
      [ -n "$deb" ] || { echo "no package owns $lib: its licence text cannot go into the notices" >&2; exit 1; }
      case "$seen" in *" $deb "*) continue ;; esac
      seen="$seen$deb "
      [ -f "/usr/share/doc/$deb/copyright" ] ||
        { echo "no /usr/share/doc/$deb/copyright: the $deb licence text is missing" >&2; exit 1; }
      set -- "$@" "$deb (bundled in lib/)=/usr/share/doc/$deb/copyright"
    done < <(ldd "$runtimes_of" | awk '/=> \// && /libc\+\+|libunwind/ { print $3 }')
    ;;
  Darwin)
    for formula in molten-vk vulkan-loader; do
      keg="$(brew --prefix "$formula")"
      found=0
      for f in "$keg"/LICENSE* "$keg"/COPYING*; do
        [ -f "$f" ] || continue
        set -- "$@" "$formula (bundled in the app)=$f"
        found=1
      done
      [ "$found" = 1 ] || { echo "no licence file in $keg: the $formula licence text is missing" >&2; exit 1; }
    done
    ;;
  *) ;; # Windows: the MSVC runtime, whose terms the summary names.
  esac
fi

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
  emit "mikktspace" "$root/src/geometry/vendor/mikktspace/LICENSE.txt"

  printf '\n### Fonts and icons\n'
  for dir in "$root"/assets/fonts/*/; do
    name=$(basename "$dir")
    files=()
    while IFS= read -r f; do files+=("$f"); done < <(find "$dir" -maxdepth 1 -type f \( -name 'LICENSE*' -o -name 'OFL*' \) | sort)
    [ ${#files[@]} -gt 0 ] || { echo "no licence file in assets/fonts/$name" >&2; exit 1; }
    emit "$name" "${files[@]}"
  done

  tr -d '\r' < "$rust_notices"

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
