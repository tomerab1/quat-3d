#!/usr/bin/env bash
# Publishes the PNGs from scripts/ci-screenshots.sh to the `ci-screenshots`
# branch so they can be viewed in the browser and fetched with plain git
# (artifact downloads are not reachable from every environment):
#
#   runs/<key>/<short-sha>/<scene>.png   key = pr-<number> or the branch name
#   runs/<key>/<short-sha>/entry.md      this run's section of the index
#   runs/index.tsv                       <epoch> <tab> <run dir>, one per run
#   index.md                             every run, newest first, images inline
#
# Commits are append-only (a new commit on top of the remote branch, never a
# force-push); a push that loses a race re-applies the run on the new tip, so
# concurrent publishes are safe.
# Retention: each publish keeps the newest KEEP_RUNS runs (default 10) per
# key and the newest KEEP_DEFAULT_RUNS (default 20) of the default branch
# (DEFAULT_BRANCH, default master); older run directories are deleted from
# the branch tip (their history stays in git until the branch is re-rooted —
# see docs/DEVELOPMENT.md). Also appends the images to $GITHUB_STEP_SUMMARY.
#
#   scripts/ci-publish-screenshots.sh [OUT_DIR]
#
# Runs inside the CI checkout (actions/checkout persists the job token as the
# git credential; the job needs `permissions: contents: write`). Required
# environment: GITHUB_REPOSITORY, GITHUB_SHA, GITHUB_RUN_ID, GITHUB_EVENT_NAME,
# GITHUB_REF_NAME. For pull requests also PR_NUMBER, HEAD_SHA (the PR head,
# not the merge commit), HEAD_REF (the PR branch) and HEAD_REPO (the head
# repository): pull requests from forks are never published (their token is
# read-only and their content untrusted) — the workflow gates on it too.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$(realpath -m "${1:-$ROOT/screenshots}")"
BRANCH=ci-screenshots
SERVER="${GITHUB_SERVER_URL:-https://github.com}"
KEEP_RUNS="${KEEP_RUNS:-10}"
KEEP_DEFAULT_RUNS="${KEEP_DEFAULT_RUNS:-20}"
DEFAULT_BRANCH="${DEFAULT_BRANCH:-master}"

die() { echo "ci-publish-screenshots: $*" >&2; exit 2; }
for v in GITHUB_REPOSITORY GITHUB_SHA GITHUB_RUN_ID GITHUB_EVENT_NAME GITHUB_REF_NAME; do
  [ -n "${!v:-}" ] || die "$v is not set"
done
[ -f "$OUT/results.tsv" ] || die "no $OUT/results.tsv (run scripts/ci-screenshots.sh first)"

# A path component from arbitrary text (branch names may hold '/').
sanitize() { printf '%s' "$1" | tr -c 'A-Za-z0-9._-' '-' | sed 's/^[-.]*//' | cut -c1-80; }

if [ "$GITHUB_EVENT_NAME" = "pull_request" ]; then
  [ -n "${PR_NUMBER:-}" ] || die "PR_NUMBER is not set"
  if [ "${HEAD_REPO:-}" != "$GITHUB_REPOSITORY" ]; then
    echo "ci-publish-screenshots: pull request from ${HEAD_REPO:-an unknown repository}; not publishing"
    exit 0
  fi
  key="pr-$(sanitize "$PR_NUMBER")"
  sha="${HEAD_SHA:-$GITHUB_SHA}"
  ref="${HEAD_REF:-$GITHUB_REF_NAME}"
else
  key="$(sanitize "$GITHUB_REF_NAME")"
  sha="$GITHUB_SHA"
  ref="$GITHUB_REF_NAME"
fi
[ -n "$key" ] || die "empty run key"
short="${sha:0:8}"
run_dir="runs/$key/$short"
run_url="$SERVER/$GITHUB_REPOSITORY/actions/runs/$GITHUB_RUN_ID"
when="$(date -u '+%Y-%m-%d %H:%M UTC')"
epoch="$(date -u +%s.%N)"

shopt -s nullglob
pngs=("$OUT"/*.png)
shopt -u nullglob

# This run's index section; images are relative to the branch root.
entry="$(mktemp)"
{
  echo "## $key @ \`$short\` — $when"
  echo
  echo "[CI run]($run_url) · ref \`$ref\` · commit [\`$short\`]($SERVER/$GITHUB_REPOSITORY/commit/$sha)"
  echo
  echo "| Scene | Result | Image |"
  echo "|---|---|---|"
  while IFS=$'\t' read -r name status seconds description; do
    if [ "$status" = "ok" ] && [ -f "$OUT/$name.png" ]; then
      img="<a href=\"$run_dir/$name.png\"><img src=\"$run_dir/$name.png\" width=\"320\" alt=\"$name\"></a>"
    else
      img="—"
    fi
    echo "| \`$name\` — $description | $status (${seconds} s) | $img |"
  done <"$OUT/results.tsv"
  echo
} >"$entry"

work="$(mktemp -d)"
cleanup() {
  git -C "$ROOT" worktree remove --force "$work" >/dev/null 2>&1 || rm -rf "$work"
  rm -f "$entry"
}
trap cleanup EXIT

# Fresh worktree on the remote tip of the branch (or a new orphan branch).
checkout_tip() {
  git -C "$ROOT" worktree remove --force "$work" >/dev/null 2>&1 || rm -rf "$work"
  if git -C "$ROOT" fetch --quiet --depth=1 origin "+refs/heads/$BRANCH:refs/remotes/origin/$BRANCH" 2>/dev/null; then
    git -C "$ROOT" worktree add --quiet --detach "$work" "origin/$BRANCH"
  else
    git -C "$ROOT" worktree add --quiet --detach "$work"
    git -C "$work" checkout --quiet --orphan "$BRANCH"
    git -C "$work" rm -r --quiet --cached . >/dev/null
    git -C "$work" clean -fdxq
  fi
}

# Copies this run in and rebuilds index.md from runs/index.tsv.
apply_run() {
  mkdir -p "$work/$run_dir"
  rm -f "$work/$run_dir"/*.png
  for p in "${pngs[@]}"; do cp "$p" "$work/$run_dir/"; done
  cp "$entry" "$work/$run_dir/entry.md"
  local tsv="$work/runs/index.tsv"
  touch "$tsv"
  # One line per run directory: a re-run of the same commit replaces its line.
  # (Stable sort with this run first: on equal timestamps it counts as newest.)
  { printf '%s\t%s\n' "$epoch" "$run_dir"; awk -F'\t' -v dir="$run_dir" '$2 != dir' "$tsv"; } \
    | sort -s -t$'\t' -k1,1gr >"$tsv.new"
  # Retention: newest KEEP_RUNS per key (runs/<key>/<sha>), KEEP_DEFAULT_RUNS
  # for the default branch; the rest are listed in $tsv.old and deleted.
  local default_key
  default_key="$(sanitize "$DEFAULT_BRANCH")"
  awk -F'\t' -v keep="$KEEP_RUNS" -v keep_default="$KEEP_DEFAULT_RUNS" \
      -v default_key="$default_key" -v kept="$tsv" -v old="$tsv.old" '
    { split($2, part, "/"); key = part[2]; n[key]++
      limit = (key == default_key) ? keep_default : keep
      if (n[key] <= limit) print > kept; else print $2 > old }' "$tsv.new"
  rm -f "$tsv.new"
  if [ -f "$tsv.old" ]; then
    while IFS= read -r dir; do
      case "$dir" in runs/*/*) rm -rf "${work:?}/$dir" ;; esac
    done <"$tsv.old"
    rm -f "$tsv.old"
  fi
  {
    echo "# CI screenshots"
    echo
    echo "Rendered by the \`linux\` job of \`.github/workflows/ci.yml\` (lavapipe under"
    echo "Xvfb, \`scripts/ci-screenshots.sh\`) for pushes and same-repository pull"
    echo "requests, and committed here by \`scripts/ci-publish-screenshots.sh\`."
    echo "Newest first. Images live under \`runs/<pr-N or branch>/<commit>/\`."
    echo
    while IFS=$'\t' read -r _ dir; do
      if [ -f "$work/$dir/entry.md" ]; then cat "$work/$dir/entry.md"; fi
    done <"$tsv"
  } >"$work/index.md"
  git -C "$work" add -A
  git -C "$work" -c user.name="github-actions[bot]" \
    -c user.email="41898282+github-actions[bot]@users.noreply.github.com" \
    commit --quiet -m "Screenshots: $key @ $short (run $GITHUB_RUN_ID)"
}

# The workflow runs publishes concurrently (no concurrency group: one would
# cancel queued publishes), so losing a push race is normal: re-apply on the
# new tip with a growing, jittered back-off so racing jobs spread out.
pushed=0
for attempt in $(seq 1 12); do
  checkout_tip
  apply_run
  if git -C "$work" push --quiet origin "HEAD:refs/heads/$BRANCH"; then
    pushed=1
    break
  fi
  echo "ci-publish-screenshots: push attempt $attempt lost a race; retrying on the new tip" >&2
  sleep $((attempt * 2 + RANDOM % 5))
done
[ "$pushed" -eq 1 ] || die "could not push to $BRANCH"
commit="$(git -C "$work" rev-parse HEAD)"
echo "Published $run_dir at $BRANCH@${commit:0:8}"

# Inline images for the run page. github.com/<repo>/raw/<commit>/<path>
# resolves for viewers signed in with access, so it also works for private
# repositories (a raw.githubusercontent.com URL would need a token there).
if [ -n "${GITHUB_STEP_SUMMARY:-}" ]; then
  {
    echo
    echo "### Images"
    echo
    echo "Branch [\`$BRANCH\`]($SERVER/$GITHUB_REPOSITORY/blob/$BRANCH/index.md) · \`$run_dir/\` · commit \`${commit:0:8}\`"
    echo
    for p in "${pngs[@]}"; do
      name="$(basename "$p" .png)"
      url="$SERVER/$GITHUB_REPOSITORY/raw/$commit/$run_dir/$name.png"
      echo "**$name**"
      echo
      echo "[![$name]($url)]($url)"
      echo
    done
  } >>"$GITHUB_STEP_SUMMARY"
fi
