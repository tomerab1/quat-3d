# Releasing

This page covers how to cut a quat-3d release. A release is a `v*` tag on
`master`. The
[release workflow](../.github/workflows/release.yml) builds, packages and
smoke-tests the game for Linux, Windows and macOS. It then publishes a GitHub
Release with the archives and the curated notes from
[`docs/releases/`](releases/). [`CHANGELOG.md`](../CHANGELOG.md) indexes the
notes.

## Versions

quat-3d uses [semantic versioning](https://semver.org). While the engine is
pre-1.0, it stays on `0.x`:

- **Minor** (`0.1.0` → `0.2.0`) for new features, and for any breaking
  change. Breaking changes include the scene file format, the remote
  JSON-RPC methods, the MCP tool names and their parameters, and the Lua
  API.
- **Patch** (`0.2.0` → `0.2.1`) for fixes that change no format or
  interface.

The version is written in three places, and all three match the tag:

- `project(quat_3d VERSION …)` in `CMakeLists.txt`;
- `version-string` in `vcpkg.json`;
- `version` in `tools/quat-mcp/Cargo.toml`, plus `Cargo.lock`. Run
  `cargo update -p quat-mcp` after the edit.

## Tags

| Tag | Meaning |
|---|---|
| `vX.Y.Z` (e.g. `v0.2.0`) | A release. Requires `docs/releases/vX.Y.Z.md`. |
| `vX.Y.Z-rc.N` (e.g. `v0.2.0-rc.1`) | A prerelease (any tag with a `-`). Notes are optional. |

The workflow refuses any other `v*` tag: it must match
`v<digits>(.<digits>)*(-<suffix>)?`. The tag name is also the version in the
archive names, the macOS `Info.plist` and the release title.

## Release notes

Every release has a curated notes file, `docs/releases/<tag>.md`. The file
is written for users, not developers. [`v0.1.0.md`](releases/v0.1.0.md) is
the model:

- a short intro;
- **Highlights**: 5–8 bullets with the big user-visible changes;
- one section per ROADMAP track (Rendering, AI tooling / MCP, Editor,
  Gameplay, Platforms & packaging, Stability & security). Each item is one
  line in user terms, with its PR numbers;
- **Platforms**: what CI builds and tests, and what the release job
  smoke-tests;
- **Getting started**, linking the README at the tag (`blob/vX.Y.Z/…`);
- **Known issues and limitations**, from `docs/STATUS.md` and the open
  issues: honest and short;
- **Third-party assets and licences** for everything the archives ship. The
  archives contain `assets/` as committed; see
  `scripts/package-release.sh`.

Rules:

- Every bullet traces to a merged PR. Gather them with
  `gh pr list --state merged --base master --search "merged:>YYYY-MM-DD"`,
  using the previous release's date. Tick the ROADMAP items they complete.
- Docs-only and CI-plumbing PRs stay out of the feature lists. CI gets one
  line at most.
- Write the notes as a PR to `master`, together with the version bump and a
  new top line in `CHANGELOG.md`.

The publish job uses the notes file as the release body. GitHub's
generated notes (the list of merged PRs and the "Full Changelog" link) are
appended below it. With both `body_path` and `generate_release_notes` set,
`softprops/action-gh-release` v3 writes the file first, then the generated
notes. A prerelease without a notes file gets the generated notes only.

## Checklist

1. **`master` is green.** The latest CI run on `master` passes on Linux,
   Windows, macOS and the Rust job. Optionally, run the release workflow by
   hand to check the packages without publishing:
   `gh workflow run release.yml --ref master`.
2. **Notes, version and changelog are merged.** The PR from
   [Release notes](#release-notes) has landed on `master`.
3. **Tag `master`'s merge commit** and push the tag:

   ```sh
   git fetch origin
   git tag -a vX.Y.Z -m "quat-3d vX.Y.Z" origin/master
   git push origin vX.Y.Z
   ```

   Check that `origin/master` is the merge commit of the notes PR, with no
   newer, untested commit on top of it.
4. **Watch the workflow** (Actions → Release). When it finishes, check the
   published release: the curated notes come first, then the generated PR
   list, and three archives are attached.

## What the workflow does

`.github/workflows/release.yml`, on a `v*` tag push:

1. **package** runs on `ubuntu-24.04`, `windows-2022` and `macos-15`, and
   each runner does the following:
   1. Checks that the notes exist (`scripts/release-notes.sh`), so a missing
      file fails within a minute rather than after the builds.
   2. Builds `game` in Release with `ENGINE_PACKAGE_BUILD=ON`, which embeds
      no source-tree paths.
   3. Validates the tag.
   4. Packages the build with `scripts/package-release.sh`.
   5. Smoke-tests the archive with `scripts/smoke-test-package.sh`. The test
      unpacks it under a path with a space and non-ASCII characters, runs 30
      frames headless, and requires a clean exit and a screenshot.
2. **publish** runs only on tags, after all three packages pass:
   1. Makes a sparse checkout of `docs/releases/` and `scripts/`, with no
      stored token.
   2. Resolves the notes again. A release tag without notes fails here with
      a clear error.
   3. Creates the GitHub Release with the archives, the curated notes and
      the generated notes. Tags with a `-` are marked as prereleases.

A run started by hand (`workflow_dispatch`) builds and smoke-tests, then
uploads the archives as workflow artifacts only. It never publishes.

## When something fails

- **Missing notes, or a build or smoke test fails:** no release is created.
  Fix the problem on `master` through a PR. Then delete the tag
  (`git push origin :refs/tags/vX.Y.Z` and `git tag -d vX.Y.Z`) and tag the
  new merge commit. Only reuse a tag name when its release was never
  published.
- **A published release is wrong:** do not move its tag. Edit the release
  text on GitHub if only the notes are wrong, and fix the notes file on
  `master` to match. For anything else, cut a patch release.
