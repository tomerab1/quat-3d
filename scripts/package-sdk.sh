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
# Size and contents report for the release log (what a game downloads).
echo "== SDK sizes"
du -sh "$stage" "$stage"/* "$stage"/deps/* "$stage"/share/quat3d/*
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
