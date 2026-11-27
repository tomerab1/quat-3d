# Learnings

Lessons from past cycles: what went wrong or cost time, and the rule that
prevents it. Every session and every worker brief reads this file. Add new
entries at the top of their section; when one keeps recurring, promote it
into CLAUDE.md or the brief template in [`CYCLE.md`](CYCLE.md).

## Agents and orchestration

- **Worktree agents start from `master`, not the session's branch.** Two of
  six agents on 2026-11-27 began on a commit predating the code they were
  meant to extend. → Every brief states the base commit and makes the agent
  verify `git log` and `git checkout -B <branch> <sha>` first.
- **Reviewers' findings must be re-verified before fixing.** Audits are
  confident even when wrong; each finding is traced in the code before work
  starts (and recorded as verified/rejected in `audit/`).
- **Give parallel agents a written contract when their work meets.** The
  engine remote API and the Rust MCP server were built in parallel against
  the method table in `remote-control.md`; write the contract first.
- **Agents must not edit STATUS.md / ROADMAP.md** — the orchestrator owns
  them; parallel edits always conflict.

## Build and CI

- **Cloud containers can't fetch vcpkg sources** (codeload.github.com is
  blocked even when allowed in the environment, as of 2026-11-27). CI is the
  source of truth for linking and tests; locally use
  `scripts/check-compile.sh` (header-only clones in `~/deps`; plain
  `git clone` from GitHub works) and run CPU tests that link without the
  heavy dependencies.
- **A push without `[skip ci]` cancels the in-progress CI run**
  (`cancel-in-progress`). While a long run is in flight, push intermediate
  commits with `[skip ci]`; let one deliberate push trigger the next run.
- **GitHub cache entries are immutable.** Save the vcpkg cache right after
  configure, with `if: always()`, under a per-run key restored by prefix —
  otherwise a failed run throws away the dependency build.
- **SDL3 (vcpkg, x11/wayland features) needs the X11/Wayland dev packages**
  on Linux: xcursor, xrandr, xi, xss, xtst, xfixes, xkbcommon,
  wayland-protocols, libdecor. The first CI run failed on xcursor.

## C++ in this codebase

- **No exceptions means nlohmann's `get<T>()` / `value()` abort on bad
  input.** Read any external JSON through `core::JsonFields`; dump with
  `error_handler_t::replace` (invalid UTF-8 also throws).
- **GoogleTest macros and commas:** `ASSERT_OK(f<a, b>())` breaks a
  non-variadic macro — the expected-result macros in `tests/support/` are
  variadic and stream extra context.
- **GoogleTest can't print `entt::null`.** Compare against a typed
  `constexpr entt::entity k_none = entt::null;`.
- **`std::span` deduction:** a template taking `std::span<const T<E>>`
  won't deduce `E` from a fixed-extent span of a C array — add a C-array
  overload.
- **Moving code between files:** extract by exact line ranges (verbatim),
  then compile-check every resulting file and check each function is
  defined exactly once — faster and safer than retyping.
- **Load into a staging object, swap on success.** Loaders that clear live
  state before validating input lose user data on a bad file (scene files).
