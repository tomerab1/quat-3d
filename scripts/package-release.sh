#!/usr/bin/env bash
# Package a release build into a self-contained archive for the current OS:
# the engine's `game` (built with -DENGINE_PACKAGE_BUILD=ON), or - with the
# options below, as quat-pack runs it - a game made with the engine, laid out
# around `quat-player` (docs/game-packaging.md). Used by
# .github/workflows/release.yml and tools/quat-pack; runs locally too (Linux,
# macOS, or Git Bash on Windows inside an MSVC developer environment).
#
#   scripts/package-release.sh <build-dir> <out-dir> <version> [options]
#     --exe <name>           executable in <build-dir> to ship (default: game)
#     --package-exe <name>   its name inside the package (default: --exe)
#     --data <dir>           data root to ship: assets/, cooked/shaders/, ...
#                            (default: the engine's assets/ + cooked/shaders/)
#     --name <name>          package base name (default: quat-3d)
#     --app-name <name>      macOS bundle name, <name>.app (default: Quat3D)
#     --display-name <name>  macOS display name (default: the app name)
#     --bundle-id <id>       macOS CFBundleIdentifier (default: dev.quat3d.editor)
#     --icon <png>           macOS app icon (converted to .icns)
#     --readme-dir <dir>     README-RUN-<os>.txt templates (default: packaging/)
#
# Layout (the executable finds its data next to itself - src/app/asset_root.hpp):
#   linux:   <name>-<v>-linux-x64/{<exe>, lib/<libc++ runtime>, <data>, README-RUN.txt}
#            -> .tar.gz; the executable's RUNPATH is $ORIGIN/lib. The Vulkan
#            loader and driver come from the system.
#   windows: <name>-<v>-windows-x64/{<exe>.exe, vcpkg DLLs, MSVC runtime DLLs, <data>, README-RUN.txt}
#            -> .zip. vulkan-1.dll comes with the GPU driver.
#   macos:   <name>-<v>-macos-arm64/{<App>.app, README-RUN.txt} -> .zip.
#            The bundle carries the Vulkan loader + MoltenVK (Contents/Frameworks,
#            ICD manifest in Contents/Resources/vulkan/icd.d) and the data in
#            Contents/Resources; ad-hoc code-signed (P7 replaces the identity).
#
# Archives are reproducible where the platform tools allow it: files carry
# SOURCE_DATE_EPOCH (default 2000-01-01) as their time, and the Linux tarball
# is sorted, owner-neutral and gzip'ed without a timestamp.
#
# Prints the archive path as the last line.
set -euo pipefail

usage() {
  sed -n '9,20p' "$0" | sed 's/^# \{0,1\}//' >&2
  exit 2
}
[ $# -ge 3 ] || usage
build_dir=$1
out_dir=$2
version=$3
shift 3
root="$(cd "$(dirname "$0")/.." && pwd)"

exe=game
package_exe=""
data_dir=""
name=quat-3d
app_name=Quat3D
display_name=""
bundle_id=dev.quat3d.editor
icon=""
readme_dir="$root/packaging"
while [ $# -gt 0 ]; do
  [ $# -ge 2 ] || usage
  case "$1" in
    --exe) exe=$2 ;;
    --package-exe) package_exe=$2 ;;
    --data) data_dir=$2 ;;
    --name) name=$2 ;;
    --app-name) app_name=$2 ;;
    --display-name) display_name=$2 ;;
    --bundle-id) bundle_id=$2 ;;
    --icon) icon=$2 ;;
    --readme-dir) readme_dir=$2 ;;
    *) echo "unknown option: $1" >&2; usage ;;
  esac
  shift 2
done
package_exe=${package_exe:-$exe}
display_name=${display_name:-$app_name}
epoch=${SOURCE_DATE_EPOCH:-946684800}

case "$(uname -s)" in
  Linux) os=linux-x64 ;;
  Darwin) os=macos-arm64 ;;
  MINGW*|MSYS*|CYGWIN*) os=windows-x64 ;;
  *) echo "unsupported OS: $(uname -s)" >&2; exit 2 ;;
esac

pkg="${name}-${version}-${os}"
mkdir -p "$out_dir"
out_dir="$(cd "$out_dir" && pwd)"
stage="$out_dir/$pkg"
rm -rf "$stage"
mkdir -p "$stage"

if [ -n "$data_dir" ]; then
  [ -d "$data_dir/cooked/shaders" ] || { echo "no cooked/shaders in $data_dir" >&2; exit 1; }
else
  [ -d "$root/cooked/shaders" ] || { echo "no cooked shaders in $root/cooked/shaders (build first)" >&2; exit 1; }
fi

# copy_data <dest>: the data root (assets/ + cooked/shaders/, plus a game's
# game.json and manifest.json).
copy_data() {
  mkdir -p "$1"
  if [ -n "$data_dir" ]; then
    cp -R "$data_dir/." "$1/"
  else
    mkdir -p "$1/cooked"
    cp -R "$root/assets" "$1/assets"
    cp -R "$root/cooked/shaders" "$1/cooked/shaders"
  fi
}

# readme <os> <dest>: the run instructions, placeholders filled in.
sed_escape() { printf '%s' "$1" | sed 's/[&|\\]/\\&/g'; }
readme() {
  sed -e "s|@NAME@|$(sed_escape "$display_name")|g" -e "s|@EXE@|$(sed_escape "$package_exe")|g" \
      -e "s|@APP@|$(sed_escape "$app_name")|g" -e "s|@MINOS@|${minos:-14.0}|g" \
      "$readme_dir/README-RUN-$1.txt" > "$2"
}

# Fixed file times, so archive bytes depend on content only.
normalize_times() {
  find "$1" -exec touch -h -d "@$epoch" {} + 2>/dev/null || find "$1" -exec touch -d "@$epoch" {} +
}

case "$os" in
linux-x64)
  cp "$build_dir/$exe" "$stage/$package_exe"
  copy_data "$stage"
  # libc++ (and what it pulls in) is not installed on most distributions:
  # ship it next to the executable. Everything else is either statically
  # linked (vcpkg's static triplet) or a base system library.
  mkdir -p "$stage/lib"
  ldd "$stage/$package_exe" | awk '/=> \// && /libc\+\+|libunwind/ { print $3 }' | while read -r lib; do
    cp -L "$lib" "$stage/lib/"
  done
  # RUNPATH is not transitive: the bundled libs find each other via $ORIGIN.
  patchelf --set-rpath '$ORIGIN/lib' "$stage/$package_exe"
  for lib in "$stage"/lib/*; do patchelf --set-rpath '$ORIGIN' "$lib"; done
  readme linux "$stage/README-RUN.txt"
  normalize_times "$stage"
  archive="$out_dir/$pkg.tar.gz"
  tar -C "$out_dir" --sort=name --owner=0 --group=0 --numeric-owner --mtime="@$epoch" \
    -cf - "$pkg" | gzip -9 -n > "$archive"
  ;;

windows-x64)
  cp "$build_dir/$exe.exe" "$stage/$package_exe.exe"
  # vcpkg's applocal step already copied the executables' DLL dependencies
  # next to them in the build directory.
  cp "$build_dir"/*.dll "$stage/"
  # MSVC runtime, app-local (no VC++ redistributable install needed): from
  # the developer environment's redist directory, else from System32 (where
  # the VC++ redistributable installs the same files).
  if [ -n "${VCToolsRedistDir:-}" ]; then
    redist="$(cygpath -u "$VCToolsRedistDir")"
    crt_dirs=("$redist"/x64/Microsoft.VC*.CRT)
    [ -d "${crt_dirs[0]}" ] || { echo "no MSVC runtime under $redist/x64" >&2; exit 1; }
    cp "${crt_dirs[0]}"/*.dll "$stage/"
  else
    sys32=$(cygpath -u "${SYSTEMROOT:-C:\\Windows}")/System32
    found=0
    for dll in msvcp140.dll msvcp140_1.dll msvcp140_2.dll vcruntime140.dll vcruntime140_1.dll concrt140.dll; do
      if [ -f "$sys32/$dll" ]; then cp "$sys32/$dll" "$stage/"; found=1; fi
    done
    [ "$found" = 1 ] || { echo "no MSVC runtime: run inside an MSVC developer environment" >&2; exit 1; }
  fi
  copy_data "$stage"
  readme windows "$stage/README-RUN.txt"
  normalize_times "$stage"
  archive="$out_dir/$pkg.zip"
  rm -f "$archive"
  (cd "$out_dir" && 7z a -tzip -bso0 -bsp0 "$archive" "$pkg")
  ;;

macos-arm64)
  app="$stage/$app_name.app"
  contents="$app/Contents"
  mkdir -p "$contents/MacOS" "$contents/Frameworks" "$contents/Resources/vulkan/icd.d"
  cp "$build_dir/$exe" "$contents/MacOS/$package_exe"
  copy_data "$contents/Resources"
  # Vulkan-on-Metal inside the bundle: the loader (linked by the executable)
  # and the MoltenVK driver. The loader searches Contents/Resources/vulkan/icd.d
  # of the main bundle; the manifest's library_path is relative to the manifest.
  mvk_prefix="$(brew --prefix molten-vk)"
  loader="$(brew --prefix vulkan-loader)/lib/libvulkan.1.dylib"
  cp -L "$loader" "$contents/Frameworks/libvulkan.1.dylib"
  cp -L "$mvk_prefix/lib/libMoltenVK.dylib" "$contents/Frameworks/libMoltenVK.dylib"
  # (No `ls a b | head` lookups in this script: under pipefail a missing
  # candidate or an early-exiting reader fails the whole command.)
  icd_src=""
  for c in "$mvk_prefix"/etc/vulkan/icd.d/MoltenVK_icd.json "$mvk_prefix"/share/vulkan/icd.d/MoltenVK_icd.json; do
    if [ -f "$c" ]; then icd_src=$c; break; fi
  done
  [ -n "$icd_src" ] || { echo "no MoltenVK ICD manifest under $mvk_prefix" >&2; exit 1; }
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
  # install_name_tool warns that this invalidates the existing ad-hoc
  # signatures; everything is re-signed below. (Stripping the signatures
  # first is not an option: codesign --remove-signature leaves a __LINKEDIT
  # layout install_name_tool refuses to edit.)
  main_bin="$contents/MacOS/$package_exe"
  install_name_tool -id @rpath/libvulkan.1.dylib "$contents/Frameworks/libvulkan.1.dylib"
  install_name_tool -id @rpath/libMoltenVK.dylib "$contents/Frameworks/libMoltenVK.dylib"
  old=$(otool -L "$main_bin" | awk '/libvulkan/ && !done { print $1; done = 1 }')
  install_name_tool -change "$old" @rpath/libvulkan.1.dylib "$main_bin"
  install_name_tool -add_rpath @executable_path/../Frameworks "$main_bin"

  # Nothing may still point into the build machine's Homebrew prefix.
  if otool -L "$main_bin" "$contents"/Frameworks/*.dylib | grep -E '/opt/homebrew|/usr/local/(opt|Cellar)'; then
    echo "bundle still references Homebrew libraries" >&2
    exit 1
  fi

  # The bundle's minimum macOS is the highest one any of its binaries was
  # built for: the executable and vcpkg ports target 14.0 (arm64-osx-14
  # triplet), Homebrew's loader/MoltenVK bottles the runner's own release.
  minos=$(for bin in "$main_bin" "$contents"/Frameworks/*.dylib; do
            otool -l "$bin" | awk '/LC_BUILD_VERSION/ { f = 1 } f && $1 == "minos" && !done { print $2; done = 1 }'
          done | sort -V | tail -n1)
  echo "bundle minimum macOS: ${minos:?could not read minos}"

  # App icon: an .icns built from the PNG with the system's image tools.
  icon_file=""
  if [ -n "$icon" ]; then
    iconset="$(mktemp -d)/AppIcon.iconset"
    mkdir -p "$iconset"
    for s in 16 32 128 256 512; do
      sips -z "$s" "$s" "$icon" --out "$iconset/icon_${s}x${s}.png" >/dev/null
      sips -z $((s * 2)) $((s * 2)) "$icon" --out "$iconset/icon_${s}x${s}@2x.png" >/dev/null
    done
    iconutil -c icns "$iconset" -o "$contents/Resources/AppIcon.icns"
    icon_file=AppIcon
  fi

  # CFBundleVersion must be numeric: v1.2.3 / 1.2.3-rc.1 -> 1.2.3, dev builds -> 0.0.0.
  plist_version=$(printf '%s' "${version#v}" | sed 's/-.*//' | grep -E '^[0-9]+(\.[0-9]+)*$' || echo 0.0.0)
  python3 -I - "$root/packaging/macos/Info.plist.in" "$contents/Info.plist" \
    "$plist_version" "$minos" "$app_name" "$display_name" "$package_exe" "$bundle_id" "$icon_file" <<'PY'
import sys
from xml.sax.saxutils import escape
src, dst, version, minos, name, display, exe, bundle_id, icon = sys.argv[1:10]
text = open(src, encoding="utf-8").read()
for key, value in {"@VERSION@": version, "@MINOS@": minos, "@NAME@": name,
                   "@DISPLAY_NAME@": display, "@EXECUTABLE@": exe,
                   "@BUNDLE_ID@": bundle_id}.items():
    text = text.replace(key, escape(value))
icon_entry = ("    <key>CFBundleIconFile</key>\n    <string>%s</string>\n" % escape(icon)) if icon else ""
text = text.replace("@ICON_ENTRY@\n", icon_entry)
open(dst, "w", encoding="utf-8").write(text)
PY
  plutil -lint "$contents/Info.plist"

  # Ad-hoc signature (no Developer ID; P7 signs for distribution): inner code
  # first, then the bundle. Apple Silicon refuses unsigned code, and
  # install_name_tool invalidated the original signatures.
  codesign --force --sign - "$contents"/Frameworks/*.dylib
  codesign --force --sign - "$app"
  codesign --verify --deep --strict "$app"

  readme macos "$stage/README-RUN.txt"
  archive="$out_dir/$pkg.zip"
  rm -f "$archive"
  ditto -c -k --keepParent "$stage" "$archive"
  ;;
esac

echo "$archive"
