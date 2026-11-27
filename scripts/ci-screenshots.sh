#!/usr/bin/env bash
# Renders a fixed set of engine scenes headlessly and converts each final
# frame to PNG (CI's `Screenshots` step; runs locally too).
#
#   scripts/ci-screenshots.sh [GAME_BINARY] [OUT_DIR]
#
# GAME_BINARY defaults to build/release/game, OUT_DIR to screenshots/. Each
# scene runs the game under Xvfb (xvfb-run) on Mesa's lavapipe with audio
# muted, a frame budget (QUAT_MAX_FRAMES + QUAT_SCREENSHOT) and a timeout, so
# a hang fails that scene instead of wedging the job. Writes to OUT_DIR:
#   <scene>.png      the final frame (downscaled 2x when >= 1280 px wide)
#   <scene>.log      the scene's stdout + stderr
#   results.tsv      scene, status (ok / crashed (exit N) / timed out / no image),
#                    seconds, description — read by ci-publish-screenshots.sh
#   summary.md       the results table, with the log tail of every failure
# summary.md is also appended to $GITHUB_STEP_SUMMARY when set. Exits 1 when
# any scene failed.
#
# Environment: VK_ICD_FILENAMES (default: the first lavapipe manifest found),
# SCENE_TIMEOUT (seconds per scene, default 300), SCENES (space-separated
# subset of the scene names below).
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GAME="$(realpath -m "${1:-$ROOT/build/release/game}")"
OUT="$(realpath -m "${2:-$ROOT/screenshots}")"
TIMEOUT="${SCENE_TIMEOUT:-300}"

# name | frames | environment (space-separated VAR=value) | description
# Frame counts: TAA history and auto-exposure settle within ~30 frames; the
# GPU terrain tile (513^2, 200 erosion steps, async readback) needs a few
# frames to arrive, and the physics scene needs simulated seconds to fall.
SCENE_TABLE=(
  "showcase-editor|30||Showcase scene with the editor UI"
  "showcase|30|QUAT_NO_UI=1|Showcase scene, no UI"
  "terrain|90|QUAT_NO_UI=1 QUAT_TERRAIN=1|Showcase + GPU-generated terrain tile"
  "physics|90|QUAT_NO_UI=1 QUAT_PHYSICS=1|Physics demo (Jolt) after 90 frames"
)

die() { echo "ci-screenshots: $*" >&2; exit 2; }

[ -x "$GAME" ] || die "game binary not found or not executable: $GAME"
command -v xvfb-run >/dev/null || die "xvfb-run not found (apt install xvfb)"
command -v python3 >/dev/null || die "python3 not found"
if [ -z "${VK_ICD_FILENAMES:-}" ]; then
  icd="$(find /usr/share/vulkan/icd.d -name 'lvp_icd*.json' 2>/dev/null | sort | head -n1)"
  [ -n "$icd" ] || die "no lavapipe ICD in /usr/share/vulkan/icd.d (apt install mesa-vulkan-drivers)"
  export VK_ICD_FILENAMES="$icd"
fi
mkdir -p "$OUT" || die "cannot create $OUT"

# The last $2 lines of log $1 without the line libc++'s libunwind prints for
# every function lavapipe's LLVM JIT registers (hundreds per run; they would
# push everything useful out of the tail).
log_tail() {
  grep -v -e '__unw_add_dynamic_fde' "$1" | tail -n "$2"
}

# PPM width from the header the engine writes ("P6\n<w> <h>\n255\n").
ppm_width() {
  local magic w _
  { read -r magic && read -r w _; } <"$1" || return 1
  [ "$magic" = "P6" ] || return 1
  printf '%s' "$w"
}

selected() {
  [ -z "${SCENES:-}" ] && return 0
  case " $SCENES " in *" $1 "*) return 0 ;; esac
  return 1
}

results="$OUT/results.tsv"
summary="$OUT/summary.md"
: >"$results"
{
  echo "## Screenshots"
  echo
  echo "Rendered on lavapipe under Xvfb (\`scripts/ci-screenshots.sh\`, ${TIMEOUT} s timeout per scene)."
  echo
  echo "| Scene | Result | Time | Setup |"
  echo "|---|---|---|---|"
} >"$summary"
failures=""
failed=0

for row in "${SCENE_TABLE[@]}"; do
  IFS='|' read -r name frames scene_env description <<<"$row"
  selected "$name" || continue
  ppm="$OUT/$name.ppm"
  png="$OUT/$name.png"
  log="$OUT/$name.log"
  rm -f "$ppm" "$png"
  echo "::group::scene $name ($frames frames${scene_env:+, $scene_env})"
  start=$SECONDS
  # shellcheck disable=SC2086 # scene_env is a list of VAR=value words
  xvfb-run -a -s "-screen 0 1920x1080x24" \
    env SDL_VIDEO_DRIVER=x11 QUAT_NO_AUDIO=1 QUAT_NO_VSYNC=1 \
        QUAT_MAX_FRAMES="$frames" QUAT_SCREENSHOT="$ppm" $scene_env \
    timeout --kill-after=15 "$TIMEOUT" "$GAME" >"$log" 2>&1
  code=$?
  elapsed=$((SECONDS - start))
  log_tail "$log" 20
  echo "::endgroup::"

  if [ "$code" -eq 124 ] || [ "$code" -eq 137 ]; then
    status="timed out"
  elif [ "$code" -ne 0 ]; then
    status="crashed (exit $code)"
  elif [ ! -s "$ppm" ]; then
    status="no image"
  else
    status="ok"
    width="$(ppm_width "$ppm")" || width=0
    factor=1
    [ "$width" -ge 1280 ] && factor=2
    if ! python3 "$ROOT/scripts/ppm_to_png.py" "$ppm" "$png" --downscale "$factor" >>"$log" 2>&1; then
      status="no image (PPM conversion failed)"
    fi
    rm -f "$ppm"
  fi

  printf '%s\t%s\t%s\t%s\n' "$name" "$status" "$elapsed" "$description" >>"$results"
  if [ "$status" != "ok" ]; then
    failed=1
    echo "::error title=Screenshot $name::$status after ${elapsed} s (log: $name.log)"
    failures+=$'\n'"<details><summary><code>$name</code>: $status — last 40 log lines</summary>"$'\n\n```\n'
    # A fence inside the log would end the code block early.
    # shellcheck disable=SC2016
    failures+="$(log_tail "$log" 40 | sed 's/```/` ` `/g')"
    failures+=$'\n```\n</details>\n'
  fi
  echo "| \`$name\` | $status | ${elapsed} s | \`${scene_env:-(defaults)}\` |" >>"$summary"
done

[ -n "$failures" ] && printf '%s\n' "$failures" >>"$summary"
[ -n "${GITHUB_STEP_SUMMARY:-}" ] && cat "$summary" >>"$GITHUB_STEP_SUMMARY"
exit "$failed"
