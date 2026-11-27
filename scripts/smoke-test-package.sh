#!/usr/bin/env bash
# Smoke-test a release archive from scripts/package-release.sh: unpack it into
# a scratch directory, launch the packaged game for 30 frames with a software
# or runner GPU, and require a clean exit plus a written screenshot. The game
# must find its data next to itself (packaged builds embed no source path).
#
#   scripts/smoke-test-package.sh <archive> <scratch-dir>
#
# Linux: Xvfb + Mesa lavapipe. Windows: the lavapipe ICD registered by the
# Vulkan SDK action. macOS: the MoltenVK bundled in the .app, with Homebrew's
# loader and MoltenVK uninstalled first so only the bundle can satisfy Vulkan.
set -euo pipefail

if [ $# -ne 2 ]; then
  echo "usage: $0 <archive> <scratch-dir>" >&2
  exit 2
fi
archive="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
scratch=$2
rm -rf "$scratch"
mkdir -p "$scratch"
scratch="$(cd "$scratch" && pwd)"
shot="$scratch/smoke.ppm"
log="$scratch/smoke.log"

export QUAT_MAX_FRAMES=30
export QUAT_SCREENSHOT="$shot"
export QUAT_NO_AUDIO=1

case "$archive" in
*linux-x64.tar.gz)
  tar -C "$scratch" -xzf "$archive"
  dir=$(ls -d "$scratch"/quat-3d-*/)
  # Everything but the system Vulkan loader must resolve, libc++ from lib/.
  deps=$(ldd "$dir/game")
  echo "$deps"
  if grep -q 'not found' <<<"$deps"; then echo "unresolved libraries" >&2; exit 1; fi
  grep -q "$dir/*lib/libc++.so" <<<"$deps" || { echo "libc++ not taken from the package" >&2; exit 1; }
  icds=(/usr/share/vulkan/icd.d/lvp_icd*.json)
  export VK_ICD_FILENAMES="${icds[0]}"
  (cd "$scratch" && xvfb-run -a -s "-screen 0 1280x720x24" "$dir/game") 2>&1 | tee "$log"
  ;;
*windows-x64.zip)
  (cd "$scratch" && 7z x -bso0 -bsp0 "$archive")
  dir=$(ls -d "$scratch"/quat-3d-*/)
  (cd "$scratch" && "$dir/game.exe") 2>&1 | tee "$log"
  ;;
*macos-arm64.zip)
  ditto -x -k "$archive" "$scratch"
  dir=$(ls -d "$scratch"/quat-3d-*/)
  app="$dir/Quat3D.app"
  codesign --verify --deep --strict "$app"
  brew uninstall --ignore-dependencies vulkan-tools molten-vk vulkan-loader >/dev/null
  export VK_LOADER_DEBUG=driver
  (cd "$scratch" && "$app/Contents/MacOS/game") 2>&1 | tee "$log"
  grep -q 'Quat3D.app/Contents/Resources/vulkan/icd.d' "$log" \
    || { echo "loader did not use the bundled MoltenVK manifest" >&2; exit 1; }
  ;;
*)
  echo "unknown archive type: $archive" >&2
  exit 2
  ;;
esac

grep -q 'data root (install directory)' "$log" \
  || { echo "game did not resolve its data next to the executable" >&2; exit 1; }
[ -s "$shot" ] || { echo "no screenshot written" >&2; exit 1; }
echo "smoke test passed: $(wc -c < "$shot") byte screenshot"
