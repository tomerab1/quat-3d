#!/usr/bin/env bash
# CTest (Linux): game packages carry the engine's licence files (#122).
# scripts/package-release.sh lays out a game (--data) with a real player
# binary and a stand-in data root, and must:
#   - refuse without engine notices (the repository's summary does not count);
#   - refuse --notices that are not full notices;
#   - ship LICENSE and the notices next to the player, byte for byte, when
#     <build-dir>/THIRD_PARTY_NOTICES.md exists (where the engine packaging
#     writes them) or --notices names them.
# With a vcpkg build tree it also generates the game notices the way the
# engine packaging does (third-party-notices.sh --no-rust --runtimes) and
# checks them: licence texts, vcpkg ports, the bundled libc++, no Rust crates.
#
#   tests/pack/package_licences_test.sh <source-root> <build-dir> <player-exe>
#
# Exit 77 (CTest SKIP_RETURN_CODE) when the packaging tools are missing.
set -euo pipefail

[ $# -eq 3 ] || { echo "usage: $0 <source-root> <build-dir> <player-exe>" >&2; exit 2; }
src=$(cd "$1" && pwd)
build=$2
player=$3
for tool in patchelf ldd tar gzip; do
  command -v "$tool" > /dev/null || { echo "skip: no $tool"; exit 77; }
done
unset QUAT_THIRD_PARTY_NOTICES

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fail() { echo "FAIL: $*" >&2; exit 1; }

# A build directory holding the player, and a game's data root.
mkdir -p "$tmp/build" "$tmp/data/cooked/shaders" "$tmp/data/assets"
cp -L "$player" "$tmp/build/player"
printf '{"format": 1}\n' > "$tmp/data/game.json"
printf 'spv\n' > "$tmp/data/cooked/shaders/stand_in.spv"
printf 'data\n' > "$tmp/data/assets/stand_in.txt"

pack() {
  bash "$src/scripts/package-release.sh" "$tmp/build" "$tmp/out" 0.0.1 \
    --exe player --package-exe sample --data "$tmp/data" --name sample \
    --app-name Sample --readme-dir "$src/packaging/game" "$@"
}

# 1. No notices next to the player, and the checkout's summary is not enough.
if pack > "$tmp/log" 2>&1; then fail "packaged a game without notices"; fi
grep -q "no engine THIRD_PARTY_NOTICES.md with licence texts" "$tmp/log" \
  || { cat "$tmp/log"; fail "no precise error for missing notices"; }
[ ! -e "$tmp/out/sample-0.0.1-linux-x64.tar.gz" ] || fail "an archive was written anyway"

# 2. --notices must be full notices.
if pack --notices "$src/THIRD_PARTY_NOTICES.md" > "$tmp/log" 2>&1; then
  fail "accepted the summary as notices"
fi
grep -q "is not a THIRD_PARTY_NOTICES.md with licence texts" "$tmp/log" \
  || { cat "$tmp/log"; fail "no precise error for summary-only notices"; }

# check_archive <notices>: LICENSE and the notices ship next to the player.
check_archive() {
  local archive="$tmp/out/sample-0.0.1-linux-x64.tar.gz"
  [ -s "$archive" ] || fail "no archive"
  rm -rf "$tmp/unpacked"
  mkdir -p "$tmp/unpacked"
  tar -C "$tmp/unpacked" -xzf "$archive"
  local dir="$tmp/unpacked/sample-0.0.1-linux-x64"
  [ -x "$dir/sample" ] || fail "no player in the package"
  cmp -s "$dir/LICENSE" "$src/LICENSE" || fail "LICENSE differs from the engine's"
  cmp -s "$dir/THIRD_PARTY_NOTICES.md" "$1" || fail "THIRD_PARTY_NOTICES.md differs from $1"
}

# 3. Notices in the build directory, where the engine packaging writes them.
printf '# Third-party notices\n\n## Licence texts\n\n### vcpkg ports\n\nstand-in\n' \
  > "$tmp/build/THIRD_PARTY_NOTICES.md"
pack > "$tmp/log" 2>&1 || { cat "$tmp/log"; fail "packaging with build-tree notices failed"; }
check_archive "$tmp/build/THIRD_PARTY_NOTICES.md"

# 4. --notices wins over the build directory's.
cp "$tmp/build/THIRD_PARTY_NOTICES.md" "$tmp/explicit.md"
printf '\nexplicit\n' >> "$tmp/explicit.md"
pack --notices "$tmp/explicit.md" > "$tmp/log" 2>&1 || { cat "$tmp/log"; fail "packaging with --notices failed"; }
check_archive "$tmp/explicit.md"

# 5. The game notices the engine packaging generates, from a real build tree.
if [ -d "$build/vcpkg_installed" ] && command -v dpkg > /dev/null; then
  bash "$src/scripts/third-party-notices.sh" --no-rust --runtimes "$player" \
    "$build" "$tmp/generated.md" > "$tmp/log" 2>&1 || { cat "$tmp/log"; fail "generating the game notices failed"; }
  grep -q '^## Licence texts' "$tmp/generated.md" || fail "generated notices have no licence texts"
  grep -q '^### vcpkg ports' "$tmp/generated.md" || fail "generated notices have no vcpkg ports"
  if ldd "$player" | grep -q 'libc++'; then
    grep -q '^### Platform runtimes' "$tmp/generated.md" || fail "the bundled libc++ has no licence text"
  fi
  if grep -q '^### quat-mcp (Rust crates)' "$tmp/generated.md"; then
    fail "--no-rust notices list quat-mcp's crates"
  fi
else
  echo "note: no vcpkg build tree at $build: generator check skipped"
fi

echo "package licences: ok"
