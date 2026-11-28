#!/usr/bin/env bash
# msl-check.sh — translate every cooked SPIR-V entry point to Metal Shading
# Language with SPIRV-Cross (the library MoltenVK embeds) and, on macOS,
# compile the result with Apple's Metal compiler.
#
# Translation bugs differ between SPIRV-Cross revisions (MoltenVK 1.4.0/1.4.1's
# pins emit `&pc.ptr` for an atomic through a push-constant pointer; 1.4.2's do
# not), so CI runs this once per MoltenVK pin with a CLI built at that pin
# (scripts/build-mvk-spirv-cross.sh) rather than with Homebrew's spirv-cross.
#
# MoltenVK reports a failed SPIR-V -> MSL conversion only as a failed
# vkCreate*Pipelines call; this names the shader, the entry point and the
# translator's (or Metal compiler's) error, without a GPU.
#
# Usage: scripts/msl-check.sh <cooked-shader-dir> [out-dir]
# SPIRV_CROSS selects the CLI (default `spirv-cross` on PATH). Needs python3;
# `xcrun metal` is used when present (REQUIRE_METAL=1 makes its absence an
# error). Exit status 1 if any entry point fails, or none were found. MSL options mirror MoltenVK's macOS defaults closely enough for
# translation errors (MSL 3.0, argument buffers); they are not a byte-exact
# reproduction of MoltenVK's output.
set -u

dir=${1:?usage: msl-check.sh <cooked-shader-dir> [out-dir]}
out=${2:-$(mktemp -d)}
mkdir -p "$out"

spirv_cross=${SPIRV_CROSS:-spirv-cross}
if ! command -v "$spirv_cross" >/dev/null 2>&1; then
    echo "msl-check: no SPIRV-Cross CLI at '$spirv_cross'" >&2
    exit 2
fi

have_metal=0
if command -v xcrun >/dev/null 2>&1 && xcrun -sdk macosx -f metal >/dev/null 2>&1; then
    have_metal=1
elif [ "${REQUIRE_METAL:-0}" = 1 ]; then
    echo "msl-check: REQUIRE_METAL=1 but xcrun metal is not available" >&2
    exit 2
fi

entry_points() { # <spv>: "name mode" lines
    "$spirv_cross" "$1" --reflect 2>/dev/null | python3 -I -c '
import json, sys
for e in json.load(sys.stdin).get("entryPoints", []):
    print(e["name"], e["mode"])'
}

total=0
failed=0
while IFS= read -r spv; do
    rel=${spv#"$dir"/}
    base=$(echo "${rel%.spv}" | tr '/' '_')
    while read -r name mode; do
        [ -n "$name" ] || continue
        total=$((total + 1))
        msl="$out/$base.$name.metal"
        if ! err=$("$spirv_cross" "$spv" --msl --msl-version 30000 --msl-argument-buffers \
                       --entry "$name" --stage "$mode" --output "$msl" 2>&1); then
            failed=$((failed + 1))
            echo "FAIL spirv-cross $rel $name ($mode): $err"
            continue
        fi
        if [ "$have_metal" = 1 ]; then
            if ! err=$(xcrun -sdk macosx metal -std=metal3.0 -c "$msl" -o "$msl.air" 2>&1); then
                failed=$((failed + 1))
                echo "FAIL metal $rel $name ($mode):"
                echo "$err" | head -n 40
                continue
            fi
        fi
        echo "ok   $rel $name ($mode)"
    done < <(entry_points "$spv")
done < <(find "$dir" -name '*.spv' | sort)

echo "msl-check: $((total - failed))/$total entry points translated$([ "$have_metal" = 1 ] && echo ' and compiled'), MSL in $out"
[ "$failed" -eq 0 ] && [ "$total" -gt 0 ]
