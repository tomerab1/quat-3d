#!/usr/bin/env bash
# Smoke-test a release archive from scripts/package-release.sh (the engine's
# `game`, or a game packaged by quat-pack): unpack it into a scratch
# directory, launch the packaged executable for 30 frames with a software or
# runner GPU, and require a clean exit plus a written screenshot. The
# executable must find its data next to itself (packaged builds embed no
# source path). Every package must carry LICENSE and a THIRD_PARTY_NOTICES.md
# with the licence texts (on macOS also inside the app's Contents/Resources).
# The engine's package (--exe game, the default) must also list quat-mcp's
# Rust crates there, and its bundled quat-mcp must run (`quat-mcp --version`);
# a game's notices must not list crates it does not ship.
#
#   scripts/smoke-test-package.sh <archive> <scratch-dir> [options]
#     --exe <name>      executable inside the package (default: game)
#     --expect <text>   a line the log must contain (repeatable)
#
# Linux: Xvfb + Mesa lavapipe. Windows: the lavapipe ICD registered by the
# Vulkan SDK action, with the DLL imports checked and a scrubbed PATH. macOS:
# the MoltenVK bundled in the .app, with Homebrew's
# loader and MoltenVK uninstalled first (CI only) so only the bundle can
# satisfy Vulkan.
set -euo pipefail

usage() {
  echo "usage: $0 <archive> <scratch-dir> [--exe <name>] [--expect <text>]..." >&2
  exit 2
}
[ $# -ge 2 ] || usage
archive="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
scratch=$2
shift 2
exe=game
expects=()
while [ $# -gt 0 ]; do
  [ $# -ge 2 ] || usage
  case "$1" in
    --exe) exe=$2 ;;
    --expect) expects+=("$2") ;;
    *) echo "unknown option: $1" >&2; usage ;;
  esac
  shift 2
done

# The engine's own package bundles quat-mcp (scripts/package-release.sh); a
# game packaged by quat-pack does not.
engine_pkg=0
[ "$exe" = game ] && engine_pkg=1
mcp_version=""

rm -rf "$scratch"
mkdir -p "$scratch"
scratch="$(cd "$scratch" && pwd)"
shot="$scratch/smoke.ppm"
log="$scratch/smoke.log"

export QUAT_MAX_FRAMES=30
export QUAT_SCREENSHOT="$shot"
export QUAT_NO_AUDIO=1

# The package folder is the archive's name without its extension.
base=$(basename "$archive")
case "$base" in
  *.tar.gz) pkg=${base%.tar.gz} ;;
  *.zip) pkg=${base%.zip} ;;
  *) echo "unknown archive type: $archive" >&2; exit 2 ;;
esac
dir="$scratch/$pkg"

# static_checks <quat-mcp path>: what the unpacked package must contain,
# checked before the launch so a failed launch still reports it.
#   - Licences: every package, engine or game, ships them (on macOS also
#     inside the app). The engine's notices list quat-mcp's crates; a game's
#     must not.
#   - The engine's package bundles quat-mcp.
static_checks() {
  if [ "$engine_pkg" = 1 ]; then
    [ -f "$1" ] || { echo "the package has no quat-mcp at $1" >&2; exit 1; }
  fi
  local licence_dirs=("${dir%/}") d f
  [ -n "${app:-}" ] && licence_dirs+=("$app/Contents/Resources")
  for d in "${licence_dirs[@]}"; do
    for f in LICENSE THIRD_PARTY_NOTICES.md; do
      [ -s "$d/$f" ] || { echo "the package has no $f in $d" >&2; exit 1; }
    done
    grep -q '^## Licence texts' "$d/THIRD_PARTY_NOTICES.md" \
      || { echo "$d/THIRD_PARTY_NOTICES.md has no licence texts" >&2; exit 1; }
    grep -q '^### vcpkg ports' "$d/THIRD_PARTY_NOTICES.md" \
      || { echo "$d/THIRD_PARTY_NOTICES.md has no vcpkg licence texts" >&2; exit 1; }
    if [ "$engine_pkg" = 1 ]; then
      grep -q '^### quat-mcp (Rust crates)' "$d/THIRD_PARTY_NOTICES.md" \
        || { echo "$d/THIRD_PARTY_NOTICES.md has no quat-mcp crate licences" >&2; exit 1; }
    elif grep -q '^### quat-mcp (Rust crates)' "$d/THIRD_PARTY_NOTICES.md"; then
      echo "$d/THIRD_PARTY_NOTICES.md lists quat-mcp's crates, which a game package does not ship" >&2
      exit 1
    fi
  done
  echo "licences: LICENSE and THIRD_PARTY_NOTICES.md in ${licence_dirs[*]}"
}

case "$archive" in
*linux-x64.tar.gz)
  tar -C "$scratch" -xzf "$archive"
  [ -d "$dir" ] || { echo "archive has no $pkg/ folder" >&2; exit 1; }
  static_checks "$dir/quat-mcp"
  # Everything but the system Vulkan loader must resolve, libc++ from lib/.
  deps=$(ldd "$dir/$exe")
  echo "$deps"
  if grep -q 'not found' <<<"$deps"; then echo "unresolved libraries" >&2; exit 1; fi
  grep -q "$dir/*lib/libc++.so" <<<"$deps" || { echo "libc++ not taken from the package" >&2; exit 1; }
  icds=(/usr/share/vulkan/icd.d/lvp_icd*.json)
  export VK_ICD_FILENAMES="${VK_ICD_FILENAMES:-${icds[0]}}"
  (cd "$scratch" && xvfb-run -a -s "-screen 0 1280x720x24" "$dir/$exe") 2>&1 | tee "$log"
  if [ "$engine_pkg" = 1 ]; then mcp_version=$("$dir/quat-mcp" --version); fi
  ;;
*windows-x64.zip)
  (cd "$scratch" && 7z x -bso0 -bsp0 "$archive")
  [ -d "$dir" ] || { echo "archive has no $pkg/ folder" >&2; exit 1; }
  static_checks "$dir/quat-mcp.exe"
  sys32=$(cygpath -u "${SYSTEMROOT:-C:\\Windows}")/System32
  # Every DLL the executable or a bundled DLL imports must be in the package
  # or a Windows system DLL; vulkan-1.dll comes with the GPU driver.
  missing=0
  # dumpbin gets relative names: it may not take a non-ASCII argument intact.
  bins=("$dir/$exe.exe")
  [ "$engine_pkg" = 1 ] && bins+=("$dir/quat-mcp.exe")
  for bin in "${bins[@]}" "$dir"/*.dll; do
    while read -r dll; do
      case "${dll,,}" in
        api-ms-*|ext-ms-*|vulkan-1.dll) continue ;;
      esac
      [ -f "$dir/$dll" ] || [ -f "$sys32/$dll" ] && continue
      echo "$(basename "$bin") imports $dll: not in the package or System32" >&2
      missing=1
    done < <(cd "$dir" && dumpbin //nologo //dependents "$(basename "$bin")" | tr -d '\r' |
               awk 'NF == 1 && /\.[Dd][Ll][Ll]$/ { print $1 }')
  done
  [ "$missing" = 0 ] || exit 1
  # Run with a scrubbed PATH (System32, Windows, the Vulkan runtime only), so
  # a DLL the package lacks cannot be picked up from the runner's tool dirs.
  clean_path="$sys32:$(dirname "$sys32")"
  [ -n "${VULKAN_SDK:-}" ] && clean_path="$clean_path:$(cygpath -u "$VULKAN_SDK")/runtime/x64"
  (cd "$scratch" && PATH="$clean_path" "$dir/$exe.exe") 2>&1 | tee "$log"
  if [ "$engine_pkg" = 1 ]; then mcp_version=$(PATH="$clean_path" "$dir/quat-mcp.exe" --version | tr -d '\r'); fi
  ;;
*macos-arm64.zip)
  ditto -x -k "$archive" "$scratch"
  [ -d "$dir" ] || { echo "archive has no $pkg/ folder" >&2; exit 1; }
  apps=("$dir"/*.app)
  app=${apps[0]}
  [ -d "$app" ] || { echo "no .app bundle in $pkg/" >&2; exit 1; }
  static_checks "$app/Contents/MacOS/quat-mcp"
  codesign --verify --deep --strict "$app"
  plutil -lint "$app/Contents/Info.plist"
  # Uninstalling Homebrew packages is only acceptable on a throwaway CI runner;
  # locally the test runs with them installed (the loader log check below
  # still requires the bundle's manifest).
  if [ "${CI:-}" = "true" ]; then
    brew uninstall --ignore-dependencies vulkan-tools molten-vk vulkan-loader >/dev/null 2>&1 || true
  else
    echo "note: not on CI - keeping Homebrew's Vulkan installed" >&2
  fi
  export VK_LOADER_DEBUG=driver
  (cd "$scratch" && "$app/Contents/MacOS/$exe") 2>&1 | tee "$log"
  grep -q "$(basename "$app")/Contents/Resources/vulkan/icd.d" "$log" \
    || { echo "loader did not use the bundled MoltenVK manifest" >&2; exit 1; }
  if [ "$engine_pkg" = 1 ]; then mcp_version=$("$app/Contents/MacOS/quat-mcp" --version); fi
  ;;
*)
  echo "unknown archive type: $archive" >&2
  exit 2
  ;;
esac

if [ "$engine_pkg" = 1 ]; then
  # The bundled MCP server starts and reports its version.
  echo "$mcp_version"
  [[ "$mcp_version" == "quat-mcp "* ]] || { echo "quat-mcp --version printed '$mcp_version'" >&2; exit 1; }
fi

grep -q 'data root (install directory)' "$log" \
  || { echo "did not resolve its data next to the executable" >&2; exit 1; }
for text in "${expects[@]+"${expects[@]}"}"; do
  grep -qF -- "$text" "$log" || { echo "log lacks: $text" >&2; exit 1; }
done
[ -s "$shot" ] || { echo "no screenshot written" >&2; exit 1; }
echo "smoke test passed: $(wc -c < "$shot") byte screenshot"
