#!/usr/bin/env bash
# build-mvk-spirv-cross.sh — build the SPIRV-Cross CLI at the revision a
# MoltenVK release embeds (its ExternalRevisions/SPIRV-Cross_repo_revision).
#
# MoltenVK translates SPIR-V to MSL with the SPIRV-Cross it pins, and
# translation bugs come and go between revisions (MoltenVK 1.4.0 / 1.4.1 emit
# `&pc.ptr` for an atomic through a push-constant pointer, which Metal
# rejects; 1.4.2 does not). Homebrew's spirv-cross follows SPIRV-Cross
# releases instead, so scripts/msl-check.sh runs with the CLI built here.
#
# Usage: scripts/build-mvk-spirv-cross.sh <moltenvk-tag> <work-dir>
#   <moltenvk-tag>  e.g. v1.4.1, or spirv-cross@<ref> for a SPIRV-Cross tag
#                   or commit directly (e.g. spirv-cross@vulkan-sdk-1.4.341.0,
#                   the revision a LunarG Vulkan SDK's MoltenVK builds with)
#   <work-dir>      SPIRV-Cross is built in <work-dir>/<revision>; a build
#                   already there is reused (CI caches the directory).
# Prints the path of the spirv-cross binary on the last line of stdout.
# Needs git, curl, cmake and ninja.
set -euo pipefail

tag=${1:?usage: build-mvk-spirv-cross.sh <moltenvk-tag> <work-dir>}
work=${2:?usage: build-mvk-spirv-cross.sh <moltenvk-tag> <work-dir>}

repo=https://github.com/KhronosGroup/SPIRV-Cross.git
if [[ $tag == spirv-cross@* ]]; then
    ref=${tag#spirv-cross@}
    if [[ $ref =~ ^[0-9a-f]{40}$ ]]; then
        rev=$ref
    else
        rev=$(git ls-remote "$repo" "refs/tags/${ref}^{}" "refs/tags/${ref}" | awk 'NR == 1 {print $1}')
    fi
else
    rev=$(curl -fsSL --retry 4 \
        "https://raw.githubusercontent.com/KhronosGroup/MoltenVK/${tag}/ExternalRevisions/SPIRV-Cross_repo_revision" |
        tr -d '[:space:]')
fi
if ! [[ $rev =~ ^[0-9a-f]{40}$ ]]; then
    echo "build-mvk-spirv-cross: ${tag} resolves to no valid SPIRV-Cross revision ('${rev}')" >&2
    exit 1
fi
echo "${tag}: SPIRV-Cross ${rev}" >&2

dir="$work/$rev"
bin="$dir/build/spirv-cross"
if [ ! -x "$bin" ]; then
    rm -rf "$dir"
    mkdir -p "$dir"
    git -C "$dir" init -q src
    git -C "$dir/src" fetch -q --depth 1 "$repo" "$rev"
    git -C "$dir/src" checkout -q FETCH_HEAD
    cmake -S "$dir/src" -B "$dir/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DSPIRV_CROSS_ENABLE_TESTS=OFF -DSPIRV_CROSS_SHARED=OFF >&2
    cmake --build "$dir/build" --target spirv-cross >&2
fi
echo "$bin"
