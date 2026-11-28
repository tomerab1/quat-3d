#!/usr/bin/env bash
# Package the engine SDK (ROADMAP P8, docs/sdk.md) from a configured and built
# release tree (-DENGINE_PACKAGE_BUILD=ON): `cmake --install` into a staging
# prefix, then archive it. Used by .github/workflows/release.yml; runs locally
# too (Linux, macOS, or Git Bash on Windows).
#
#   scripts/package-sdk.sh <build-dir> <out-dir> <version>
#
# Archives: quat3d-sdk-<version>-<os>.tar.gz (Linux, macOS) or .zip (Windows),
# holding one top-level directory of the same name: the SDK prefix
# (include/, lib/, lib/cmake/quat3d/, bin/, deps/, share/quat3d/).
# Prints the archive path as the last line.
set -euo pipefail

if [ $# -ne 3 ]; then
  echo "usage: $0 <build-dir> <out-dir> <version>" >&2
  exit 2
fi
build_dir=$1
out_dir=$2
version=$3

case "$(uname -s)" in
  Linux) os=linux-x64 ;;
  Darwin) os=macos-arm64 ;;
  MINGW*|MSYS*|CYGWIN*) os=windows-x64 ;;
  *) echo "unsupported OS: $(uname -s)" >&2; exit 2 ;;
esac

root="$(cd "$(dirname "$0")/.." && pwd)"
name="quat3d-sdk-${version}-${os}"
mkdir -p "$out_dir"
out_dir="$(cd "$out_dir" && pwd)"
stage="$out_dir/$name"
rm -rf "$stage"
cmake --install "$build_dir" --prefix "$stage" > /dev/null

for required in lib/cmake/quat3d/quat3dConfig.cmake lib/cmake/quat3d/quat3d-toolchain.cmake \
                include/engine/version.hpp share/quat3d/cooked/shaders deps; do
  [ -e "$stage/$required" ] || { echo "SDK stage lacks $required" >&2; exit 1; }
done
ls "$stage"/bin

# Licences (#96, #122). Two full notices files, generated from this build by
# scripts/third-party-notices.sh, which ships with the release-notes work:
#   <sdk>/THIRD_PARTY_NOTICES.md              everything the SDK carries: the
#       engine, every vcpkg port in deps/, the fonts, and quat-mcp's crates
#       when bin/ has quat-mcp;
#   <sdk>/share/quat3d/THIRD_PARTY_NOTICES.md the --no-rust notices quat-pack
#       copies into game packages (the engine root's file; a game carries no
#       quat-mcp, so it must not list its crates).
# Each must have the "## Licence texts" section (quat-pack ignores a summary),
# and LICENSE must sit beside each. Without the generator in the tree:
#   - a release (a tag push: GITHUB_REF_TYPE=tag, or QUAT_SDK_RELEASE=1)
#     fails, because a published SDK never ships without its notices;
#   - a dev build or a workflow_dispatch dry run packages the SDK without
#     them and says so.
# Both also carry the licences of the platform runtimes a game package
# bundles next to the player (libc++ on Linux, MoltenVK and the Vulkan loader
# on macOS): --runtimes reads them off the build's quat-player.
notices_tool="$root/scripts/third-party-notices.sh"
if [ -f "$notices_tool" ]; then
  rust_flag=()
  [ -e "$stage/bin/quat-mcp" ] || [ -e "$stage/bin/quat-mcp.exe" ] || rust_flag=(--no-rust)
  runtimes=()
  for player in "$build_dir/quat-player" "$build_dir/quat-player.exe"; do
    if [ -f "$player" ]; then runtimes=(--runtimes "$player"); break; fi
  done
  [ ${#runtimes[@]} -gt 0 ] || { echo "no quat-player in $build_dir: its runtime licences are needed for the notices" >&2; exit 1; }
  bash "$notices_tool" ${rust_flag[@]+"${rust_flag[@]}"} "${runtimes[@]}" "$build_dir" "$stage/THIRD_PARTY_NOTICES.md"
  bash "$notices_tool" --no-rust "${runtimes[@]}" "$build_dir" "$stage/share/quat3d/THIRD_PARTY_NOTICES.md"
  for dir in "$stage" "$stage/share/quat3d"; do
    grep -q '^## Licence texts' "$dir/THIRD_PARTY_NOTICES.md" ||
      { echo "$dir/THIRD_PARTY_NOTICES.md has no licence texts" >&2; exit 1; }
    [ -s "$dir/LICENSE" ] || { echo "no engine LICENSE in $dir" >&2; exit 1; }
  done
elif [ "${GITHUB_REF_TYPE:-}" = "tag" ] || [ "${QUAT_SDK_RELEASE:-0}" = "1" ]; then
  echo "release SDK: $notices_tool is missing, and a release must ship its licence notices" >&2
  exit 1
else
  echo "note: $notices_tool not in this tree (#96): SDK packaged without generated notices (not a release)" >&2
fi
# Size and contents report for the release log (what a game downloads).
echo "== SDK sizes"
# One du call per level: du counts a directory once, so listing a parent
# with its children would print the parent alone.
du -sh "$stage"
du -sh "$stage"/*
du -sh "$stage"/deps/*
du -sh "$stage"/share/quat3d/*
echo "== deps/share ports"
ls "$stage"/deps/share
echo "== deps/lib"
ls "$stage"/deps/lib

case "$os" in
  windows-x64)
    archive="$out_dir/$name.zip"
    (cd "$out_dir" && 7z a -tzip -bso0 -bsp0 "$archive" "$name")
    ;;
  *)
    archive="$out_dir/$name.tar.gz"
    tar -C "$out_dir" -czf "$archive" "$name"
    ;;
esac
rm -rf "$stage"
echo "$archive"
