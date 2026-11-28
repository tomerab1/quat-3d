#!/usr/bin/env bash
# Smoke-test a release archive from scripts/package-release.sh (the engine's
# `game`, or a game packaged by quat-pack): unpack it into a scratch
# directory, launch the packaged executable for 30 frames with a software or
# runner GPU, and require a clean exit plus a written screenshot. The
# executable must find its data next to itself (packaged builds embed no
# source path).
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

# macOS: prints the crashed and main threads of the newest crash report for
# <exe> written after <marker> (ReportCrash needs a few seconds). Never fails:
# under set -e / pipefail a missing report directory or an unexpected report
# shape must not end the script before the caller exits with the crash status.
print_macos_crash_report() {
  local exe=$1 marker=$2 dir="$HOME/Library/Logs/DiagnosticReports" report="" _
  for _ in $(seq 1 30); do
    report=$( { find "$dir" -maxdepth 1 -name "$exe*.ips" -newer "$marker" 2>/dev/null || true; } |
                sort | tail -n1)
    [ -n "$report" ] && break
    sleep 1
  done
  if [ -z "$report" ]; then
    echo "no crash report for $exe in $dir" >&2
    return 0
  fi
  echo "---- crash report: $report" >&2
  python3 - "$report" >&2 <<'PY' || echo "could not summarise $report" >&2
import json
import sys

text = open(sys.argv[1], encoding="utf-8", errors="replace").read()
_, _, body = text.partition("\n")  # line 1: header object; then the report
try:
    report = json.loads(body)
except ValueError:
    print(text[:20000])
    sys.exit(0)
images = report.get("usedImages", [])
print("exception:", json.dumps(report.get("exception", {})))
faulting = report.get("faultingThread", 0)
for index, thread in enumerate(report.get("threads", [])):
    if index not in (0, faulting):
        continue
    label = "crashed" if index == faulting else "main"
    print(f"thread {index} ({label}) {thread.get('name', '')} {thread.get('queue', '')}")
    for n, frame in enumerate(thread.get("frames", [])[:64]):
        image = frame.get("imageIndex", -1)
        name = images[image].get("name", "?") if 0 <= image < len(images) else "?"
        symbol = frame.get("symbol", hex(frame.get("imageOffset", 0)))
        print(f"  #{n:<2} {name:<28} {symbol} + {frame.get('symbolLocation', 0)}")
PY
}

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

case "$archive" in
*linux-x64.tar.gz)
  tar -C "$scratch" -xzf "$archive"
  [ -d "$dir" ] || { echo "archive has no $pkg/ folder" >&2; exit 1; }
  # Everything but the system Vulkan loader must resolve, libc++ from lib/.
  deps=$(ldd "$dir/$exe")
  echo "$deps"
  if grep -q 'not found' <<<"$deps"; then echo "unresolved libraries" >&2; exit 1; fi
  grep -q "$dir/*lib/libc++.so" <<<"$deps" || { echo "libc++ not taken from the package" >&2; exit 1; }
  icds=(/usr/share/vulkan/icd.d/lvp_icd*.json)
  export VK_ICD_FILENAMES="${VK_ICD_FILENAMES:-${icds[0]}}"
  (cd "$scratch" && xvfb-run -a -s "-screen 0 1280x720x24" "$dir/$exe") 2>&1 | tee "$log"
  ;;
*windows-x64.zip)
  (cd "$scratch" && 7z x -bso0 -bsp0 "$archive")
  [ -d "$dir" ] || { echo "archive has no $pkg/ folder" >&2; exit 1; }
  sys32=$(cygpath -u "${SYSTEMROOT:-C:\\Windows}")/System32
  # Every DLL the executable or a bundled DLL imports must be in the package
  # or a Windows system DLL; vulkan-1.dll comes with the GPU driver.
  missing=0
  # dumpbin gets relative names: it may not take a non-ASCII argument intact.
  for bin in "$dir/$exe.exe" "$dir"/*.dll; do
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
  status=0
  (cd "$scratch" && PATH="$clean_path" "$dir/$exe.exe") 2>&1 | tee "$log" || status=$?
  if [ "$status" != 0 ]; then
    # Git Bash reports most crashes (an unhandled NTSTATUS such as
    # 0xC0000409, MSVC's fast-fail after abort / an exception in a build
    # without them) as 127, and the first run's real code is not visible
    # here. Run it once more through PowerShell, which reports it — a rerun:
    # a flaky failure may pass there (0x00000000). It runs in its own
    # directory, so the first run's log and screenshot stay as they were.
    mkdir -p "$scratch/rerun"
    # shellcheck disable=SC2016 # PowerShell expands these
    code=$(cd "$scratch/rerun" || exit 0
           PATH="$clean_path:$sys32/WindowsPowerShell/v1.0" \
             QUAT_SCREENSHOT="$scratch/rerun/smoke.ppm" \
             QUAT_SMOKE_EXE="$(cygpath -w "$dir/$exe.exe")" powershell.exe -NoProfile -Command \
             '$p = Start-Process $env:QUAT_SMOKE_EXE -Wait -PassThru -NoNewWindow; "0x{0:X8}" -f $p.ExitCode' \
             2>/dev/null | tr -d '\r' | tail -n1 || true)
    echo "$exe.exe failed: status $status under Git Bash (crashes show as 127);" \
         "a rerun through PowerShell exited with ${code:-unknown}" >&2
    exit "$status"
  fi
  ;;
*macos-arm64.zip)
  ditto -x -k "$archive" "$scratch"
  [ -d "$dir" ] || { echo "archive has no $pkg/ folder" >&2; exit 1; }
  apps=("$dir"/*.app)
  app=${apps[0]}
  [ -d "$app" ] || { echo "no .app bundle in $pkg/" >&2; exit 1; }
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
  touch "$scratch/.launched"
  status=0
  (cd "$scratch" && "$app/Contents/MacOS/$exe") 2>&1 | tee "$log" || status=$?
  if [ "$status" != 0 ]; then
    # A crash leaves only "Segmentation fault: 11" here; ReportCrash writes
    # the backtrace to a .ips report a moment later.
    print_macos_crash_report "$exe" "$scratch/.launched"
    exit "$status"
  fi
  grep -q "$(basename "$app")/Contents/Resources/vulkan/icd.d" "$log" \
    || { echo "loader did not use the bundled MoltenVK manifest" >&2; exit 1; }
  ;;
*)
  echo "unknown archive type: $archive" >&2
  exit 2
  ;;
esac

grep -q 'data root (install directory)' "$log" \
  || { echo "did not resolve its data next to the executable" >&2; exit 1; }
for text in "${expects[@]+"${expects[@]}"}"; do
  grep -qF -- "$text" "$log" || { echo "log lacks: $text" >&2; exit 1; }
done
[ -s "$shot" ] || { echo "no screenshot written" >&2; exit 1; }
echo "smoke test passed: $(wc -c < "$shot") byte screenshot"
