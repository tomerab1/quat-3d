#!/usr/bin/env bash
# Syntax + warning check of engine translation units WITHOUT vcpkg: compiles
# each given file with the engine's flags against header-only clones of the
# dependencies (see docs/DEVELOPMENT.md, "Compile-checking without vcpkg").
# Catches compile errors and warnings — not link errors or test failures.
#
#   scripts/check-compile.sh src/scene/scene.cpp tests/scene/*_test.cpp
#
# DEPS (default ~/deps) must hold: entt, vma, jolt, fastgltf, stb, sdl,
# googletest, miniaudio, imgui, imgui-node-editor, ktx (KTX-Software),
# meshoptimizer, clipper2 (Clipper2) and earcut (earcut.hpp) (cloned at the
# vcpkg-pinned versions), acl and rtm (the tags cmake/FetchAcl.cmake pins).
set -u
D="${DEPS:-$HOME/deps}"
ROOT="$(git rev-parse --show-toplevel)"
cd "$ROOT" || exit 2
fail=0
# check_one <file> [extra flags...]: one compile; prints "ok" or the errors.
check_one() {
  local f=$1
  shift
  # Mirror CMake: the path macros exist only for the game target and the
  # tests, so library code that uses them fails here as it does in CI.
  local paths=()
  case "$f" in src/app/*|src/main.cpp|tests/*) paths=(-DQUAT_COOKED_SHADER_DIR='"x"' -DQUAT_PROJECT_ROOT='"x"');; esac
  case "$f" in tests/*) paths+=(-DQUAT_SHADER_SOURCE_DIR='"x"');; esac
  local out
  out=$(clang++ -std=c++23 -stdlib=libc++ -Wall -Wextra -Wshadow -fno-exceptions -fno-rtti \
    -fsyntax-only -DGLM_FORCE_DEPTH_ZERO_TO_ONE -DGLM_FORCE_RADIANS -DENGINE_EDITOR \
    "${paths[@]}" "$@" \
    -Iinclude -Isrc -Isrc/remote -Itests \
    -isystem "$D/entt/src" -isystem "$D/vma/include" -isystem "$D/jolt" \
    -isystem "$D/fastgltf/include" -isystem "$D/stb" -isystem "$D/sdl/include" \
    -isystem "$D/miniaudio" -isystem "$D/imgui" -isystem "$D/imgui/backends" -isystem "$D/imgui/misc/cpp" -isystem "$D/imgui-node-editor" -isystem "$D/recast-inc" -isystem "$D/googletest/googletest/include" -isystem "$D/acl/includes" -isystem "$D/rtm/includes" -isystem "$D/ktx/include" -isystem "$D/meshoptimizer/src" -isystem "$D/clipper2/CPP/Clipper2Lib/include" -isystem "$D/earcut/include" -isystem src/geometry/vendor/mikktspace ${EXTRA_FLAGS:-} \
    "$f" 2>&1)
  local what="$f"
  [ $# -gt 0 ] && what="$f ($*)"
  if [ -n "$out" ]; then echo "== $what"; echo "$out" | head -40; fail=1; else echo "ok $what"; fi
}

for f in "$@"; do
  check_one "$f"
  # The application layer is also compiled into the players (P4): without
  # the editor (quat-player) and with remote control compiled out
  # (quat-player-shipping), neither with the source-tree path macros.
  case "$f" in
    src/app/*|src/player/*)
      check_one "$f" -UENGINE_EDITOR -UQUAT_PROJECT_ROOT -UQUAT_COOKED_SHADER_DIR
      check_one "$f" -UENGINE_EDITOR -DQUAT_SHIPPING -UQUAT_PROJECT_ROOT -UQUAT_COOKED_SHADER_DIR
      ;;
  esac
done
exit $fail
