# Learnings

Lessons from past cycles: what went wrong or cost time, and the rule that
prevents it. Every session and every worker brief reads this file. Add new
entries at the top of their section; when one keeps recurring, promote it
into CLAUDE.md or the brief template in [`CYCLE.md`](CYCLE.md).

## Agents and orchestration

- **Unpushed worktree commits die with the container.** Worker agents push
  their `feat/<id>` branch to origin after every commit (owner's rule,
  2026-11-27). `feat/**` pushes don't trigger CI, so they never cancel a
  running build.
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

- **Lavapipe's ICD file is `lvp_icd.json` on Ubuntu 24.04** (not
  `lvp_icd.x86_64.json`) — a wrong `VK_ICD_FILENAMES` gives
  `VK_ERROR_INCOMPATIBLE_DRIVER` from vkCreateInstance. CI globs for it.
  Lavapipe also runs in the cloud container (`apt install mesa-vulkan-drivers`
  or `/opt/lavapipe/lvp_icd.json`), so GPU tests can run locally.
- **Order-dependent / layout-dependent failures are memory bugs.** A test
  that fails only after another one, or passes under gdb (no ASLR), is
  reading invalid memory: run it under valgrind, which also instruments
  third-party static libraries that ASan builds don't (it found an OOB read
  inside fastgltf).
- **CI triggered on both `push` to `claude/**` and `pull_request` ran every
  commit twice.** Branch pushes now only build `master`; cycle branches build
  through their PR.
- **The owner may merge a cycle PR before CI finishes.** After a merge, merge
  `master` back into the cycle branch (no rewriting), open the next cycle's
  draft PR, and watch `master`'s CI — a red `master` is the next cycle's
  first item.
- **GPU tests can run locally without vcpkg.** The container has lavapipe
  (`VK_ICD_FILENAMES=/opt/lavapipe/lvp_icd.json`) and `libvulkan`; the RHI,
  asset and most renderer TUs only need the `~/deps` header clones (VMA,
  googletest) + `/usr/include` glm/nlohmann. Compile just the TUs a test
  needs (rhi/*.cpp incl. `vma_impl.cpp` with `-w`, the passes under test,
  `tests/support/gpu_context.cpp`, `tests/test_main.cpp`), link
  `libgtest.a -lvulkan`, and cook shaders with slangc into a scratch dir
  passed as `QUAT_COOKED_SHADER_DIR` (R10.1 ran its `Gpu*` suites this way).
  Avoid `scene::upload_mesh` (fastgltf) in tests that should run locally.
- **Compile-check the whole tree once, not only changed files.** The first
  full CI build failed on a pre-existing `std::jthread` use that no
  per-file check had touched; one `scripts/check-compile.sh $(git ls-files
  'src/*.cpp')` pass found it in a minute (missing-header failures for
  ImGui / GNS / Recast are expected locally). → Run it before every push
  that triggers CI.
- **libc++ 18 (Ubuntu 24.04 clang) gates `std::jthread`/`stop_token`**
  behind `-fexperimental-library`. Use `std::thread` + explicit join.
- **Subagent pushes can be blocked by the permission system** even when the
  owner asked for pushes (the agent's own context lacks the owner's
  instruction). The orchestrator pushes `feat/*` branches itself when a
  worker reports a blocked push.
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

- **slangc 2025.6.1 segfaults after reporting a compile error** when
  `-reflection-json` is passed (exit by SIGSEGV, diagnostics already
  printed). Treat any non-zero exit *or* signal as a failed compile and keep
  the captured output as the diagnostics (the R2 compiler service does).
- **A compute pipeline whose shader declares a binding its layout lacks can
  crash lavapipe inside vkCreateComputePipelines** (no validation layer in
  the container to catch it first). Hot reload therefore checks reflected
  bindings against the original layout before rebuilding a pipeline.

- **Don't let a parser touch untrusted indices before validation.**
  fastgltf's `Options::GenerateMeshIndices` dereferences accessor indices
  during parsing; the loader now validates first and generates sequential
  indices itself. Same for `fastgltf::validate`: it is stricter than real
  exporter files — validate exactly what the loader dereferences.
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
