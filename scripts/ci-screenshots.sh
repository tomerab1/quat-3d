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
#   <scene>.png      the final frame (downscaled 2x when >= 1280 px wide,
#                    except editor scenes: their UI text must stay readable)
#   <scene>.log      the scene's stdout + stderr
#   results.tsv      scene, status (ok / crashed (exit N) / timed out / no image),
#                    seconds, description — read by ci-publish-screenshots.sh
#   summary.md       the results table, with the log tail of every failure
# summary.md is also appended to $GITHUB_STEP_SUMMARY when set. Exits 1 when
# any scene failed.
#
# Environment: VK_ICD_FILENAMES (default: the first lavapipe manifest found),
# SCENE_TIMEOUT (seconds per scene, default 300; a row may raise its own with
# SCENE_TIMEOUT=<s> in its environment column), SCENES (space-separated
# subset of the scene names below).
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GAME="$(realpath -m "${1:-$ROOT/build/release/game}")"
OUT="$(realpath -m "${2:-$ROOT/screenshots}")"
TIMEOUT="${SCENE_TIMEOUT:-300}"

# name | frames | environment (space-separated VAR=value) | description
#      | asset manifest (optional)
# Frame counts: TAA history and auto-exposure settle within ~30 frames; the
# GPU terrain tile (513^2, 200 erosion steps, async readback) needs a few
# frames to arrive, and the physics scene needs simulated seconds to fall.
# mountain-forest: its tile (400 erosion steps + talus relaxation) lands by frame ~6, then ~11k
# scattered instances + grass draw at ~2-3 s per frame on lavapipe (4 cores):
# 60-100 s, inside the default timeout.
# Editor rows run at 1600x900 from the default layout (QUAT_EDITOR_LAYOUT;
# scripts/editor-screenshots.sh captures more editor states) and are never
# downscaled.
# A scene built from downloaded assets names its manifest: the row is
# skipped (not failed) when scripts/fetch-polyhaven.py --check finds the
# assets missing, e.g. because Poly Haven was unreachable.
SCENE_TABLE=(
  "showcase-editor|30|QUAT_WINDOW_SIZE=1600x900 QUAT_EDITOR_LAYOUT=default|Showcase scene with the editor UI (1600x900)"
  "editor-inspector|30|QUAT_WINDOW_SIZE=1600x900 QUAT_EDITOR_LAYOUT=default QUAT_EDITOR_SELECT=metal_sphere|Editor with an entity selected (Inspector)"
  "showcase|30|QUAT_NO_UI=1|Showcase scene, no UI"
  "terrain|90|QUAT_NO_UI=1 QUAT_TERRAIN=1|Showcase + GPU-generated terrain tile"
  "physics|90|QUAT_NO_UI=1 QUAT_PHYSICS=1|Physics demo (Jolt) after 90 frames"
  "cinematic|16|SCENE_TIMEOUT=900 QUAT_NO_UI=1 QUAT_PLAY=1 QUAT_GAME_CAMERA=1 QUAT_WINDOW_SIZE=960x540 QUAT_SCENE=assets/scenes/cinematic_forest.json|Cinematic forest clearing at golden hour (CC0 Poly Haven assets), hero camera|assets/showcase/cinematic/assets.json"
  # World clock (R22.1): Paris on the 2026 June solstice, the clock frozen.
  # Sunrise 05:47, solar noon 13:52, sunset 21:58 local (UTC+2).
  "sunrise|30|QUAT_NO_UI=1 QUAT_TIME=2026-06-21T06:10:00+02:00 QUAT_LATLON=48.8566,2.3522 QUAT_TIME_SCALE=0|World clock: Paris, June solstice, 06:10 (sun 2.5° up, north-east)"
  "noon|30|QUAT_NO_UI=1 QUAT_TIME=2026-06-21T13:52:00+02:00 QUAT_LATLON=48.8566,2.3522 QUAT_TIME_SCALE=0|World clock: Paris, June solstice, solar noon (sun 64.6° up, south)"
  "sunset|30|QUAT_NO_UI=1 QUAT_TIME=2026-06-21T21:35:00+02:00 QUAT_LATLON=48.8566,2.3522 QUAT_TIME_SCALE=0|World clock: Paris, June solstice, 21:35 (sun 2.5° up, north-west)"
  "editor-time-of-day|30|QUAT_WINDOW_SIZE=1600x900 QUAT_EDITOR_LAYOUT=default QUAT_EDITOR_SELECT=world_clock QUAT_TIME=2026-06-21T21:35:00+02:00 QUAT_TIME_SCALE=0|Editor: the world clock in the Inspector at sunset"
  "showcase-hdri|40|QUAT_NO_UI=1 QUAT_HDRI=hdri-cache/kloofendal_48d_partly_cloudy_2k.hdr|Showcase lit by a CC0 Poly Haven HDRI (R21: HDRI IBL, its sun as the shadowed light, SSR)"
  "mountain-forest|30|QUAT_NO_UI=1 QUAT_DEMO=mountain|Mountain + forest showcase: terrain, scattered trees and rocks, GPU grass"
  # Layered terrain material (R26): meadow hill with rock ridges and dirt in
  # the gullies; and a fir forest on forest-floor ground (hashed alpha
  # foliage, R26.3). The texture sets come from assets/terrain/layers.json
  # (untextured layers fall back to their tinted colour).
  "terrain-materials|40|QUAT_NO_UI=1 QUAT_PLAY=1 QUAT_GAME_CAMERA=1 QUAT_SCENE=assets/scenes/terrain_materials.json|Terrain materials (R26.1): meadow hill, rock ridges, dirt in gullies, GPU grass"
  "terrain-forest|30|SCENE_TIMEOUT=900 QUAT_NO_UI=1 QUAT_PLAY=1 QUAT_GAME_CAMERA=1 QUAT_WINDOW_SIZE=960x540 QUAT_SCENE=assets/scenes/terrain_forest.json|Fir forest on forest-floor terrain (R26.1 + hashed alpha foliage R26.3)|assets/showcase/cinematic/assets.json"
)
# HDRI panoramas the scenes use, fetched into hdri-cache/ (gitignored).
HDRIS=("kloofendal_48d_partly_cloudy 2k")

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
# A download failure is not fatal: the HDRI scene then renders the procedural
# sky and its log says why.
for hdri in "${HDRIS[@]}"; do
  # shellcheck disable=SC2086 # "<id> <resolution>"
  "$ROOT/scripts/fetch-hdri.sh" $hdri >/dev/null || echo "::warning::could not fetch HDRI $hdri"
done

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
  echo "Rendered on lavapipe under Xvfb (\`scripts/ci-screenshots.sh\`, ${TIMEOUT} s timeout per scene unless a scene sets more)."
  echo
  echo "| Scene | Result | Time | Setup |"
  echo "|---|---|---|---|"
} >"$summary"
failures=""
failed=0

for row in "${SCENE_TABLE[@]}"; do
  IFS='|' read -r name frames scene_env description manifest <<<"$row"
  selected "$name" || continue
  if [ -n "$manifest" ] &&
     ! python3 "$ROOT/scripts/fetch-polyhaven.py" --check "$ROOT/$manifest" >"$OUT/$name.log" 2>&1; then
    echo "::warning title=Screenshot $name skipped::assets of $manifest not fetched (scripts/fetch-polyhaven.py)"
    printf '%s\t%s\t%s\t%s\n' "$name" "skipped (assets not fetched)" 0 "$description" >>"$results"
    echo "| \`$name\` | skipped: assets not fetched | 0 s | \`$scene_env\` |" >>"$summary"
    continue
  fi
  row_timeout="$TIMEOUT"
  for kv in $scene_env; do
    case "$kv" in SCENE_TIMEOUT=*) row_timeout="${kv#SCENE_TIMEOUT=}" ;; esac
  done
  ppm="$OUT/$name.ppm"
  png="$OUT/$name.png"
  log="$OUT/$name.log"
  rm -f "$ppm" "$png"
  echo "::group::scene $name ($frames frames${scene_env:+, $scene_env})"
  start=$SECONDS
  # shellcheck disable=SC2086 # scene_env is a list of VAR=value words
  xvfb-run -a -s "-screen 0 1920x1080x24" \
    env SDL_VIDEO_DRIVER=x11 QUAT_NO_AUDIO=1 QUAT_NO_VSYNC=1 QUAT_NON_FINITE_CHECK=1 \
        QUAT_MAX_FRAMES="$frames" QUAT_SCREENSHOT="$ppm" $scene_env \
    timeout --kill-after=15 "$row_timeout" "$GAME" >"$log" 2>&1
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
    # scripts/check_capture.py: no exact-black block (a NaN/Inf spreading
    # through the HDR chain) and, for editor scenes, the editor UI really
    # drawn (not blank, uniform or a 3D view without ImGui).
    check_args=()
    [[ "$name" == *editor* ]] && check_args=(--editor)
    if ! python3 "$ROOT/scripts/check_capture.py" "${check_args[@]}" "$ppm" >>"$log" 2>&1; then
      status="bad capture: $(grep '^check_capture:' "$log" | tail -n1 | sed 's/^[^:]*: [^:]*: //')"
    fi
    width="$(ppm_width "$ppm")" || width=0
    factor=1
    # Editor captures stay at full size: halved, the panels' text is
    # unreadable. The window is a fixed 1280x720 for now.
    # TODO(E8, #52): once it lands, render the editor row with E8's startup
    # hooks (QUAT_WINDOW_SIZE=1600x900, QUAT_EDITOR_LAYOUT=default, or its
    # scripts/editor-screenshots.sh) instead of the default window.
    [[ "$name" != *editor* ]] && [ "$width" -ge 1280 ] && factor=2
    if ! python3 "$ROOT/scripts/ppm_to_png.py" "$ppm" "$png" --downscale "$factor" >>"$log" 2>&1; then
      status="no image (PPM conversion failed)"
    fi
    rm -f "$ppm"
  fi

  # QUAT_NON_FINITE_CHECK=1: the engine logs when its bloom / TAA guards had
  # to replace NaN/Inf values (a source upstream to fix, issue #45).
  if grep -q '^\[warn\] non-finite' "$log"; then
    echo "::warning title=Non-finite values in $name::$(grep -m1 '^\[warn\] non-finite' "$log")"
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
