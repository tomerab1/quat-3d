#!/usr/bin/env bash
# Package a release build of `game` (built with -DENGINE_PACKAGE_BUILD=ON) into
# a self-contained archive for the current OS. Used by .github/workflows/
# release.yml; runs locally too (Linux, macOS, or Git Bash on Windows inside
# an MSVC developer environment).
#
#   scripts/package-release.sh <build-dir> <out-dir> <version>
#
# Layout (the game finds its data next to the executable — src/app/asset_root.hpp):
#   linux:   quat-3d-<v>-linux-x64/{game, lib/<libc++ runtime>, assets/, cooked/shaders/, README-RUN.txt}
#            -> .tar.gz; game's RUNPATH is $ORIGIN/lib. The Vulkan loader and
#            driver come from the system.
#   windows: quat-3d-<v>-windows-x64/{game.exe, vcpkg DLLs, MSVC runtime DLLs, assets/, cooked/shaders/, README-RUN.txt}
#            -> .zip. vulkan-1.dll comes with the GPU driver.
#   macos:   quat-3d-<v>-macos-arm64/{Quat3D.app, README-RUN.txt} -> .zip.
#            The bundle carries the Vulkan loader + MoltenVK (Contents/Frameworks,
#            ICD manifest in Contents/Resources/vulkan/icd.d) and the data in
#            Contents/Resources; ad-hoc code-signed.
#
# Prints the archive path as the last line.
set -euo pipefail

if [ $# -ne 3 ]; then
  echo "usage: $0 <build-dir> <out-dir> <version>" >&2
  exit 2
fi
build_dir=$1
out_dir=$2
version=$3
root="$(cd "$(dirname "$0")/.." && pwd)"

case "$(uname -s)" in
  Linux) os=linux-x64 ;;
  Darwin) os=macos-arm64 ;;
  MINGW*|MSYS*|CYGWIN*) os=windows-x64 ;;
  *) echo "unsupported OS: $(uname -s)" >&2; exit 2 ;;
esac

name="quat-3d-${version}-${os}"
mkdir -p "$out_dir"
out_dir="$(cd "$out_dir" && pwd)"
stage="$out_dir/$name"
rm -rf "$stage"
mkdir -p "$stage"

[ -d "$root/cooked/shaders" ] || { echo "no cooked shaders in $root/cooked/shaders (build first)" >&2; exit 1; }

# copy_data <dest>: assets/ + cooked/shaders/ (the data root layout).
copy_data() {
  mkdir -p "$1/cooked"
  cp -R "$root/assets" "$1/assets"
  cp -R "$root/cooked/shaders" "$1/cooked/shaders"
}

case "$os" in
linux-x64)
  cp "$build_dir/game" "$stage/game"
  copy_data "$stage"
  # libc++ (and what it pulls in) is not installed on most distributions:
  # ship it next to the game. Everything else is either statically linked
  # (vcpkg's static triplet) or a base system library.
  mkdir -p "$stage/lib"
  ldd "$stage/game" | awk '/=> \// && /libc\+\+|libunwind/ { print $3 }' | while read -r lib; do
    cp -L "$lib" "$stage/lib/"
  done
  # RUNPATH is not transitive: the bundled libs find each other via $ORIGIN.
  patchelf --set-rpath '$ORIGIN/lib' "$stage/game"
  for lib in "$stage"/lib/*; do patchelf --set-rpath '$ORIGIN' "$lib"; done
  cp "$root/packaging/README-RUN-linux.txt" "$stage/README-RUN.txt"
  archive="$out_dir/$name.tar.gz"
  tar -C "$out_dir" -czf "$archive" "$name"
  ;;

windows-x64)
  cp "$build_dir/game.exe" "$stage/"
  # vcpkg's applocal step already copied the game's DLL dependencies next to
  # game.exe in the build directory.
  cp "$build_dir"/*.dll "$stage/"
  # MSVC runtime, app-local (no VC++ redistributable install needed).
  redist="$(cygpath -u "${VCToolsRedistDir:?run inside an MSVC developer environment}")"
  crt_dir=$(ls -d "$redist"/x64/Microsoft.VC*.CRT | head -n1)
  cp "$crt_dir"/*.dll "$stage/"
  copy_data "$stage"
  cp "$root/packaging/README-RUN-windows.txt" "$stage/README-RUN.txt"
  archive="$out_dir/$name.zip"
  (cd "$out_dir" && 7z a -tzip -bso0 -bsp0 "$archive" "$name")
  ;;

macos-arm64)
  app="$stage/Quat3D.app"
  contents="$app/Contents"
  mkdir -p "$contents/MacOS" "$contents/Frameworks" "$contents/Resources/vulkan/icd.d"
  cp "$build_dir/game" "$contents/MacOS/game"
  copy_data "$contents/Resources"
  # Vulkan-on-Metal inside the bundle: the loader (linked by the game) and the
  # MoltenVK driver. The loader searches Contents/Resources/vulkan/icd.d of the
  # main bundle; the manifest's library_path is relative to the manifest.
  mvk_prefix="$(brew --prefix molten-vk)"
  loader="$(brew --prefix vulkan-loader)/lib/libvulkan.1.dylib"
  cp -L "$loader" "$contents/Frameworks/libvulkan.1.dylib"
  cp -L "$mvk_prefix/lib/libMoltenVK.dylib" "$contents/Frameworks/libMoltenVK.dylib"
  icd_src=$(ls "$mvk_prefix"/etc/vulkan/icd.d/MoltenVK_icd.json "$mvk_prefix"/share/vulkan/icd.d/MoltenVK_icd.json 2>/dev/null | head -n1)
  python3 -I - "$icd_src" "$contents/Resources/vulkan/icd.d/MoltenVK_icd.json" <<'PY'
import json, sys
src, dst = sys.argv[1], sys.argv[2]
with open(src) as f:
    m = json.load(f)
m["ICD"]["library_path"] = "../../../Frameworks/libMoltenVK.dylib"
with open(dst, "w") as f:
    json.dump(m, f, indent=4)
PY

  chmod u+w "$contents"/Frameworks/*.dylib
  install_name_tool -id @rpath/libvulkan.1.dylib "$contents/Frameworks/libvulkan.1.dylib"
  install_name_tool -id @rpath/libMoltenVK.dylib "$contents/Frameworks/libMoltenVK.dylib"
  old=$(otool -L "$contents/MacOS/game" | awk '/libvulkan/ { print $1; exit }')
  install_name_tool -change "$old" @rpath/libvulkan.1.dylib "$contents/MacOS/game"
  install_name_tool -add_rpath @executable_path/../Frameworks "$contents/MacOS/game"

  # Nothing may still point into the build machine's Homebrew prefix.
  if otool -L "$contents/MacOS/game" "$contents"/Frameworks/*.dylib | grep -E '/opt/homebrew|/usr/local/(opt|Cellar)'; then
    echo "bundle still references Homebrew libraries" >&2
    exit 1
  fi

  # The bundle's minimum macOS is the highest one any of its binaries was
  # built for: the game and vcpkg ports target 14.0 (arm64-osx-14 triplet),
  # Homebrew's loader/MoltenVK bottles the runner's own release.
  minos=$(for bin in "$contents/MacOS/game" "$contents"/Frameworks/*.dylib; do
            otool -l "$bin" | awk '/LC_BUILD_VERSION/ { f = 1 } f && $1 == "minos" { print $2; exit }'
          done | sort -V | tail -n1)
  echo "bundle minimum macOS: ${minos:?could not read minos}"
  # CFBundleVersion must be numeric: tags v1.2.3 -> 1.2.3, dev builds -> 0.0.0.
  plist_version=$(printf '%s' "${version#v}" | grep -E '^[0-9]+(\.[0-9]+)*$' || echo 0.0.0)
  sed -e "s/@VERSION@/$plist_version/g" -e "s/@MINOS@/$minos/g" \
    "$root/packaging/macos/Info.plist.in" > "$contents/Info.plist"

  # Ad-hoc signature (no Developer ID): inner code first, then the bundle.
  # Apple Silicon refuses unsigned code, and install_name_tool invalidated the
  # original signatures.
  codesign --force --sign - "$contents"/Frameworks/*.dylib
  codesign --force --sign - "$app"
  codesign --verify --deep --strict "$app"

  cp "$root/packaging/README-RUN-macos.txt" "$stage/README-RUN.txt"
  archive="$out_dir/$name.zip"
  ditto -c -k --keepParent "$stage" "$archive"
  ;;
esac

echo "$archive"
