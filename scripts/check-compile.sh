#!/usr/bin/env bash
# Syntax + warning check of engine translation units WITHOUT vcpkg: compiles
# each given file with the engine's flags against header-only clones of the
# dependencies (see docs/DEVELOPMENT.md, "Compile-checking without vcpkg").
# Catches compile errors and warnings — not link errors or test failures.
#
#   scripts/check-compile.sh src/scene/scene.cpp tests/scene/*_test.cpp
#
# DEPS (default ~/deps) must hold: entt, vma, jolt, fastgltf, stb, sdl,
# googletest, miniaudio, imgui, imgui-node-editor (cloned at the vcpkg-pinned
# versions), acl and rtm (the tags cmake/FetchAcl.cmake pins).
set -u
D="${DEPS:-$HOME/deps}"
ROOT="$(git rev-parse --show-toplevel)"
cd "$ROOT" || exit 2
fail=0
for f in "$@"; do
  out=$(clang++ -std=c++23 -stdlib=libc++ -Wall -Wextra -Wshadow -fno-exceptions -fno-rtti \
    -fsyntax-only -DGLM_FORCE_DEPTH_ZERO_TO_ONE -DGLM_FORCE_RADIANS -DENGINE_EDITOR \
    -DQUAT_COOKED_SHADER_DIR='"x"' -DQUAT_PROJECT_ROOT='"x"' \
    -Iinclude -Isrc -Isrc/remote -Itests \
    -isystem "$D/entt/src" -isystem "$D/vma/include" -isystem "$D/jolt" \
    -isystem "$D/fastgltf/include" -isystem "$D/stb" -isystem "$D/sdl/include" \
    -isystem "$D/miniaudio" -isystem "$D/imgui" -isystem "$D/imgui/backends" -isystem "$D/imgui/misc/cpp" -isystem "$D/imgui-node-editor" -isystem "$D/recast-inc" -isystem "$D/googletest/googletest/include" -isystem "$D/acl/includes" -isystem "$D/rtm/includes" ${EXTRA_FLAGS:-} \
    "$f" 2>&1)
  if [ -n "$out" ]; then echo "== $f"; echo "$out" | head -40; fail=1; else echo "ok $f"; fi
done
exit $fail
