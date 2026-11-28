#!/usr/bin/env bash
# Downloads a CC0 HDRI panorama from Poly Haven into hdri-cache/ (gitignored)
# and checks it against the MD5 the API publishes:
#
#   scripts/fetch-hdri.sh kloofendal_48d_partly_cloudy_puresky [1k|2k|4k]
#
# Prints the project-relative path to pass as QUAT_HDRI or to
# environment.set {"hdri": {"path": ...}}. Already-downloaded files that
# match the checksum are not fetched again.
set -euo pipefail
id="${1:?usage: fetch-hdri.sh <polyhaven-hdri-id> [1k|2k|4k]}"
res="${2:-2k}"
case "$id" in *[!A-Za-z0-9_-]*) echo "bad HDRI id: $id" >&2; exit 2 ;; esac
case "$res" in 1k|2k|4k|8k) ;; *) echo "bad resolution: $res" >&2; exit 2 ;; esac
root="$(cd "$(dirname "$0")/.." && pwd)"
out_dir="$root/hdri-cache"
out="$out_dir/${id}_${res}.hdr"
mkdir -p "$out_dir"
meta="$(curl -fsSL "https://api.polyhaven.com/files/$id")"
read -r url md5 < <(python3 -I -c '
import json, sys
f = json.loads(sys.stdin.read())["hdri"][sys.argv[1]]["hdr"]
print(f["url"], f["md5"])' "$res" <<<"$meta")
case "$url" in https://dl.polyhaven.org/*) ;; *) echo "unexpected download host: $url" >&2; exit 1 ;; esac
if [ -f "$out" ] && [ "$(md5sum "$out" | cut -d' ' -f1)" = "$md5" ]; then
  echo "hdri-cache/${id}_${res}.hdr"
  exit 0
fi
curl -fsSL "$url" -o "$out.part"
if [ "$(md5sum "$out.part" | cut -d' ' -f1)" != "$md5" ]; then
  rm -f "$out.part"
  echo "checksum mismatch for $url" >&2
  exit 1
fi
mv "$out.part" "$out"
echo "hdri-cache/${id}_${res}.hdr"
