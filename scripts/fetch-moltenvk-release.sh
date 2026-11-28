#!/usr/bin/env bash
# fetch-moltenvk-release.sh — download a MoltenVK GitHub release's macOS
# dylib and print a Vulkan ICD manifest for it, so the GPU tests can run on a
# MoltenVK other than Homebrew's (VK_ICD_FILENAMES=<printed path>).
#
# Usage: scripts/fetch-moltenvk-release.sh <tag> <work-dir>
#   <tag>       e.g. v1.4.1, or "latest"
#   <work-dir>  the release is unpacked into <work-dir>/<tag>
# Prints the ICD manifest path on the last line of stdout. Needs `gh`
# (GH_TOKEN set) and tar.
set -euo pipefail

tag=${1:?usage: fetch-moltenvk-release.sh <tag> <work-dir>}
work=${2:?usage: fetch-moltenvk-release.sh <tag> <work-dir>}
repo=KhronosGroup/MoltenVK

if [ "$tag" = latest ]; then
    tag=$(gh release view -R "$repo" --json tagName -q .tagName)
fi
dir="$work/$tag"
rm -rf "$dir"
mkdir -p "$dir"

# Recent releases ship MoltenVK-macos.tar; older ones only MoltenVK-all.tar.
if ! gh release download "$tag" -R "$repo" -D "$dir" -p 'MoltenVK-macos.tar' >&2; then
    gh release download "$tag" -R "$repo" -D "$dir" -p 'MoltenVK-all.tar' >&2
fi
for t in "$dir"/*.tar; do tar -xf "$t" -C "$dir"; done

dylib=$(find "$dir" -path '*macOS*' -name libMoltenVK.dylib -type f | head -n1)
if [ -z "$dylib" ]; then
    echo "fetch-moltenvk-release: no macOS libMoltenVK.dylib in $repo $tag" >&2
    exit 1
fi
icd="$(dirname "$dylib")/MoltenVK_icd.json"
if [ ! -f "$icd" ]; then
    cat >"$icd" <<'JSON'
{
    "file_format_version": "1.0.0",
    "ICD": {
        "library_path": "./libMoltenVK.dylib",
        "api_version": "1.4.0",
        "is_portability_driver": true
    }
}
JSON
fi
echo "MoltenVK $tag: $dylib" >&2
echo "$icd"
