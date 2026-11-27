#!/usr/bin/env bash
# Captures the docked editor headlessly in a fixed set of states (ROADMAP E8:
# real editor screenshots for before/after comparisons and CI).
#
#   scripts/editor-screenshots.sh [GAME_BINARY] [OUT_DIR] [PREFIX]
#
# GAME_BINARY defaults to build/release/game (an ENGINE_EDITOR=ON build),
# OUT_DIR to screenshots/editor, PREFIX to "editor" (files are
# <PREFIX>-<state>.png). Each state runs the game under Xvfb on Mesa's
# lavapipe with QUAT_WINDOW_SIZE (default 1600x900), QUAT_MAX_FRAMES and
# QUAT_SCREENSHOT, which copies the presented swapchain image -- the whole
# editor window with ImGui drawn, not just the scene viewport. The editor's
# startup hooks pick the state: QUAT_EDITOR_SELECT=<entity name> selects an
# entity, QUAT_EDITOR_FOCUS=<panel title> brings a docked panel to the front.
# A saved layout would change the captures, so QUAT_EDITOR_LAYOUT=default
# makes each run start from the default layout without touching the user's.
#
# Environment: VK_ICD_FILENAMES (default: the first lavapipe manifest found),
# EDITOR_SHOT_SIZE (WxH), EDITOR_SHOT_FRAMES (default 24), STATES (subset of
# the state names below), EDITOR_SHOT_ENV (extra VAR=value words for every
# state, e.g. QUAT_EDITOR_THEME=light). Exits 1 when any state failed.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GAME="$(realpath -m "${1:-$ROOT/build/release/game}")"
OUT="$(realpath -m "${2:-$ROOT/screenshots/editor}")"
PREFIX="${3:-editor}"
SIZE="${EDITOR_SHOT_SIZE:-1600x900}"
FRAMES="${EDITOR_SHOT_FRAMES:-24}"
TIMEOUT="${SCENE_TIMEOUT:-300}"

# state | environment (space-separated VAR=value)
STATE_TABLE=(
  "default|"
  "inspector|QUAT_EDITOR_SELECT=metal_sphere"
  "assets|QUAT_EDITOR_FOCUS=Assets"
  "behaviour|QUAT_EDITOR_SELECT=metal_sphere QUAT_EDITOR_FOCUS=Behaviour"
)

die() { echo "editor-screenshots: $*" >&2; exit 2; }
[ -x "$GAME" ] || die "game binary not found or not executable: $GAME"
command -v xvfb-run >/dev/null || die "xvfb-run not found (apt install xvfb)"
if [ -z "${VK_ICD_FILENAMES:-}" ]; then
  icd="$(find /usr/share/vulkan/icd.d /opt/lavapipe -name 'lvp_icd*.json' 2>/dev/null | sort | head -n1)"
  [ -n "$icd" ] || die "no lavapipe ICD found (apt install mesa-vulkan-drivers)"
  export VK_ICD_FILENAMES="$icd"
fi
mkdir -p "$OUT" || die "cannot create $OUT"
screen_w="${SIZE%x*}"
screen_h="${SIZE#*x}"

failed=0
for row in "${STATE_TABLE[@]}"; do
  IFS='|' read -r state state_env <<<"$row"
  if [ -n "${STATES:-}" ]; then
    case " $STATES " in *" $state "*) ;; *) continue ;; esac
  fi
  ppm="$OUT/$PREFIX-$state.ppm"
  png="$OUT/$PREFIX-$state.png"
  log="$OUT/$PREFIX-$state.log"
  rm -f "$ppm" "$png"
  # shellcheck disable=SC2086 # the env lists are VAR=value words
  xvfb-run -a -s "-screen 0 $((screen_w + 64))x$((screen_h + 64))x24" \
    env SDL_VIDEO_DRIVER=x11 QUAT_NO_AUDIO=1 QUAT_NO_VSYNC=1 QUAT_EDITOR_LAYOUT=default \
        QUAT_WINDOW_SIZE="$SIZE" QUAT_MAX_FRAMES="$FRAMES" QUAT_SCREENSHOT="$ppm" \
        $state_env ${EDITOR_SHOT_ENV:-} \
    timeout --kill-after=15 "$TIMEOUT" "$GAME" >"$log" 2>&1
  code=$?
  if [ "$code" -ne 0 ] || [ ! -s "$ppm" ]; then
    echo "editor-screenshots: $state failed (exit $code, log $log)" >&2
    failed=1
    continue
  fi
  python3 "$ROOT/scripts/ppm_to_png.py" "$ppm" "$png" && rm -f "$ppm"
  echo "wrote $png"
done
exit "$failed"
