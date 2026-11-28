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

- **A clean auto-merge can still break the build.** Merging master (#20)
  into the E8 branch raised no conflict, but master had renamed
  `WorldRuntime::clear_spin_entity` while the branch added a new call to it;
  the 40-minute forced rebuild failed late. → After any merge, compile-check
  the TUs both sides touched (`scripts/check-compile.sh` on the merge's file
  list) before starting a long build.
- **Hand-rolled local builds must rebuild dependents of changed headers.**
  The E8 worker's scratch build script recompiled a `.cpp` only when it was
  newer than its object; after a member was added to `EditorShell`, stale
  objects disagreed on its size and the game aborted in `free()` at exit
  (heap corruption, not a logic bug). → Scratch scripts rebuild every TU of a
  module when any of its headers is newer than the object (or use `FORCE=1`);
  a crash right after a header change is a stale-object suspect first.
- **The editor can be seen headless.** `QUAT_SCREENSHOT` copies the presented
  swapchain image, UI included; `scripts/editor-screenshots.sh` runs the
  game under Xvfb + lavapipe at 1600x900 with `QUAT_EDITOR_SELECT` /
  `QUAT_EDITOR_FOCUS` / `QUAT_EDITOR_LAYOUT=default` to pose it. CI's
  `showcase-editor` image was never broken, only useless: 1280x720 halved to
  640x360. Editor captures are taken at 1600x900 or larger without
  downscaling; the remote `view.screenshot` deliberately returns only the
  viewport image.
- **A full game build without vcpkg works in the container**: header clones
  in `~/deps` plus prebuilt `libJolt.a` / `libfastgltf.a` / SDL3 / GNS, and
  ImGui, ImGuizmo (tag 1.10), imgui-node-editor and Recast compiled from
  source, linked with `-lSDL3 -lGameNetworkingSockets -lvulkan`. It takes
  20-40 minutes of shared CPU from scratch; captures are then
  `scripts/editor-screenshots.sh <game> <dir> <prefix>`.
- **Don't tie the hitch clamp to the physics budget.** D2 first capped the
  sim frame dt at 1/15 s (8 physics substeps), so on slow machines (CI's
  lavapipe at 1-3 fps) the whole world ran several times slower than real
  time. The clamp is now 0.25 s (only true hitches are dropped) and frames
  above 1/15 s are sliced for physics; 1/15 s bounds only `debug.step`.
- **Half-float HDR targets overflow on mirror highlights (#45).** A punctual
  GGX peak of 1/(pi alpha^2) exceeds 65504 for alpha < ~0.003; some drivers
  round to +Inf (CI's lavapipe), others saturate (the container's), so the
  bug only showed in CI. Floor punctual roughness (0.089), clamp HDR writes
  (`lib/hdr.slang`), and test overflow with `<` 65504, not `<=`.

- **Look at every render you report on (issue #21).** A worker reported the
  physics demo "unchanged" after #7/#8 without opening the image; every box
  had fallen out of the world. Cause: `physics_system` creates bodies at
  `Transform.world`, and on frame 0 only `FlyCamera::create`'s `scene.tick()`
  had resolved those — #8 skipped it for the physics demo. → Systems resolve
  the transforms they read (`WorldRuntime::simulate` now does, every slice),
  and `tests/app/` drives the real `WorldRuntime` path (`game_world` library).

- **The real `game` runs locally under Xvfb + lavapipe.** Without vcpkg, a
  non-editor build links from the `~/deps` clones: build SDL3
  (`~/deps/sdl`, cmake, shared) and link libJolt / libfastgltf / GNS /
  Recast sources; then `scripts/ci-screenshots.sh <game>` renders the CI
  scenes (about 0.7 s per frame on 4 cores). libc++'s libunwind prints
  `__unw_add_dynamic_fde: bad fde` for every function lavapipe's JIT
  registers — hundreds of lines per run — so filter it before reading a log
  tail (the screenshot script does).
- **`git ls-files` + `check-compile.sh` catches includes lost in merges.**
  `skinning_pass.cpp` used `glm::mat4` with no glm include after S3.3
  removed the self-test that pulled it in.
- **Link the game executable locally after any merge that touches library
  targets.** R17 linked libktx into `game` directly while engine_asset only
  compiled against its headers; the tests (linking the full libktx) passed,
  but after the G8 merge engine_asset was re-emitted after `libktx_read.a`
  on the game's link line and GNU ld's single pass left the KTX2 symbols
  undefined (CI run 46028587322). A library now owns its archive dependency
  (engine_asset links libktx through a per-executable `QUAT_KTX_FULL`
  switch), and without vcpkg a stub prefix (`find_package` configs over the
  `~/deps` builds) is enough to configure and link `game` / `engine_tests`.

- **Local test builds must track header dependencies.** A scratch build that
  rebuilt only changed .cpp files linked objects compiled against an old
  class layout and crashed (R12). Compile with `-MMD -MF` and rebuild when
  any listed dependency is newer than the object.
- **Hand-compiling translation units proves neither that they are in a CMake
  target nor that they link.** G8.4 passed every `-fsyntax-only` check, but
  `game` failed to link: `compress_clips` was declared in
  `scene::gltf_detail` (internal.hpp) and defined in `scene`. → Before
  pushing, grep CMakeLists for every new `.cpp`, and for a new cross-file
  function compile caller and callee with `-c` and compare `nm` (the
  undefined symbol must match the defined one).
- **Local `-c` object caches go stale across merges.** A GPU test failed with
  "no Vulkan device" after master changed `rhi::Device`: unchanged test
  sources kept objects built against the old header. Rebuild from scratch
  after merging.

- **`--validation` silently runs without the layer when it isn't
  installed.** The container lacks the Khronos layer;
  `apt-get install -y vulkan-validationlayers` adds it (R10.2 ran its GPU
  tests under it, plus `VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT`
  for sync validation).
- **Parallel subagents share one scratchpad directory.** On 2026-11-27 two
  workers wrote `cc.sh` / `cook.sh` / `shaders/` at the scratchpad root and
  overwrote each other's build scripts (one script `cd`s into its own
  worktree, so the other would have compiled the wrong tree). → Each worker
  keeps its files in its own subdirectory (`scratchpad/<branch-or-agent>/`).
- **Jolt, Recast and GNS tests can run locally.** Jolt: build
  `~/deps/jolt/Build` with clang + `-stdlib=libc++`, `CPP_RTTI_ENABLED=OFF`,
  `CPP_EXCEPTIONS_ENABLED=OFF`, and compile the engine TUs with the same
  `JPH_*` defines and `-mavx2 -mbmi -mpopcnt -mlzcnt -mf16c -mfma` (read them
  from the generated `build.ninja`) plus `-DNDEBUG` (else `JPH::AssertFailed`
  is unresolved). Recast/Detour: compile `~/deps/recast/*/Source/*.cpp`
  directly. GNS: `apt install libprotobuf-dev protobuf-compiler libssl-dev`,
  build v1.4.1 as a shared library (default libstdc++ is fine — its API has
  no std types) and link the libc++ tests against it with an rpath.
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

- **Optimising a fixed-step system must not change its floats (#84).** Nav
  feeds network prediction, so the separation grid had to match the
  all-pairs loop bit for bit. Two traps: float sums depend on order (sort
  the neighbours back into view order), and kinematic agents move while the
  same loop is still running (the grid re-files each one as it moves). →
  Keep the old loop behind a reference switch that only tests can reach (a
  private field plus `friend struct NavSeparationTestAccess`, defined in
  `tests/`, as #90 did) and compare whole-registry state
  bytewise tick after tick. Mutating the new code (dropping the sort, the
  re-file, or the cell headroom) must make that test fail. The nav tests
  link locally without Jolt: recast sources plus a stub
  `collect_nav_geometry` for box colliders.
- **Explicit thermal erosion is diffusion: a slope N cells long needs ~N²
  steps** (issue #69). The interior of a uniform ramp gives as much as it
  receives, so it only relaxes from its crest and toe; 400 steps left 55-75°
  faces on a 513² tile whatever the rate. Run it to its steady state with
  multigrid (talus_relax.cpp), and solve the coarsest level to convergence:
  with only 8 sweeps there, a uniform 60° ramp stayed at 59°, because every
  level sees the same ramp. Test with a ramp, not just a spike.
- **A multigrid cascade is not monotone just because its smoother is**
  (PR #90 review). Restriction by decimation turned a one-texel spike on an
  even index into a coarse plateau, and its correction, interpolated onto
  neighbours that never held material, dug 5 m pits. Restrict with the
  volume-weighted transpose of the prolongation (full weighting; [1 1]/2 on
  borders, where a sample is half a cell), weight the sweeps by the same
  volumes so borders conserve too, and test range and volume with spikes on
  even and odd texels, borders and corners.
- **A `std::async` future can be ready while its worker is still running.**
  The promise is fulfilled inside the lambda, so `wait_for(0)` on the
  task's own `future<void>` may still report "not ready" just after the
  result arrived. Tests that count finished workers must poll (#78).
- **A parameter range check per field is not a budget.** Terrain fields were
  each in range, but tiles x resolution² reached 78 GB (#67). When several
  untrusted fields multiply into memory or time, check the product, and
  have the error name the largest values that fit.

- **Designated initializers + `-Wextra` warn for every omitted field that
  has no default member initializer** (clang's
  `-Wmissing-field-initializers`). Option structs meant for `{.a = 1}` call
  sites (the editor's `PropOpts`, `Command`, `DragSpec`) give every member a
  default (`std::string id{};`), or each call site warns.
- **Inter (and other UI faces) put stylistic alternates in the Unicode
  Private Use Area**, where icon fonts live. Merging an icon font into it
  without `ImFontConfig::GlyphExcludeRanges = {0xE000, 0xF8FF}` on the text
  face makes some icons render as alternate digits / letters (E8: Inspector's
  icon showed "3", Stats' showed "ǒ").
- **ImGui docking: the focused window's tab stays in front.** New windows take
  focus as they appear, so the last panel submitted wins its tab group no
  matter what the DockBuilder said. Select tabs through the dock node's tab
  bar and move focus elsewhere (the viewport) for the first frames after
  building a layout.
- **An ImGui settings handler's `UserData` must outlive the context, or the
  ini must be written first.** `DestroyContext` saves the ini through every
  handler; the editor saves explicitly, clears `IniFilename`, then destroys
  the shell the handler points at.
- **A render-graph image is one unit: a pass that changes per-mip layouts
  must hand every mip back in the layout its declared usage implies.** The
  transparent pass's scene-colour copy blitted its mip chain and left mips
  1.. in SHADER_READ_ONLY while the graph, whose barriers cover all mips,
  still believed TRANSFER_DST; its next transition then lied about the old
  layout (#62, five `VUID-VkImageMemoryBarrier2-oldLayout-01197` per frame).
  Restore the declared layout at the end of the pass and let the graph do
  the hand-off.
- **Slang keeps a dead `if (!A::keep(...)) discard;` after specialising.**
  An interface method returning a constant `true` still left `OpKill` in the
  opaque entry point's SPIR-V (R12.11). Put the discard inside the
  interface implementation (`AlphaMasked::test`) so the opaque
  specialisation has none, and check entry points' call graphs for
  OpKill / OpTerminateInvocation / OpDemoteToHelperInvocation in a test.
- **A material sampler created with `create_sampler(device)` reaches mip 1
  only** (maxLod = mip_levels = 1): mipmapped material textures were never
  minified past level 1 on master. Pass `material_sampler_max_lod`.
- **Coverage-preserving alpha mips must be judged on the stored 8-bit
  alpha.** A threshold picked from float sub-samples sat within half an
  8-bit step of whole stems of equal alpha, and rounding flipped them all
  (+9 % coverage). Evaluate the stored coverage at a few 8-bit steps around
  the analytic threshold and keep the closest.
- **`SV_IsFrontFace` is screen winding, not "outside".** A mirrored instance
  (negative determinant) winds its outside faces clockwise, so a doubleSided
  normal flip keyed on front_face alone lit mirrored foliage from behind
  (PR #70 review). Use `front_face != mirrored`, with `sign(det M)` from
  InstanceData, and test a negatively scaled instance.
- **Post-decode processing of untrusted textures must be costed before it
  allocates.** The first coverage-mip pass reserved its budget only after a
  float copy of level 0 plus a vector chain (> 1 GiB for an 8k image). Size
  the result and the working set up front, claim them from the DecodeBudget,
  read level 0 in place, keep two float levels alive, and write straight into
  the final buffer; prove it with an allocation probe (tests/support).
- **A shadow term that is filtered apart from the depth PCF draws its own
  hard edge** (issue #77). Glass shadows were looked up with a 2x2 gather of
  a half-resolution map, after the PCSS-filtered depth test, so their edge
  stair-stepped; and the angle-dependent Fresnel at the sphere's grazing
  silhouette wrote black texels (the dump showed transmittance 0.0 there),
  a dark ring. Multiply each PCF tap's depth test by that tap's
  transmittance, let translucent depth join the PCSS blocker search, and use
  the hemispherical average Fresnel (1 - F_avg is what a sphere transmits
  over its projected area) because the map cannot model the caustic that
  balances grazing reflection. Debug by dumping the map (`read_rgba16f` on
  `CsmShared::translucent_map`) before guessing.
- **Count volume absorption once per volume, not per face.** A shadow
  caster sees both faces of closed glass; KHR_materials_volume `thickness` is
  the whole medium, so Beer-Lambert belongs on the light's entry face only.
- **Import the shader-side contract instead of copying it.** A third copy of
  the material flag bits and PbrMaterialParams in transparent.slang would
  desync silently; passes import `material`, and a test checks the flag bits
  and every pass's `g_params` reflection offsets against the C++ struct.
- **A line protocol on a loopback port is reachable from every web page**
  (issue #56). Browsers send cross-origin `POST`s to `127.0.0.1` without a
  CORS preflight, and HTTP is lines: the remote endpoint answered the request
  line and headers with parse errors, kept reading, and ran the body as an
  RPC call — unauthenticated, because the token was optional and off by
  default. Loopback is not a trust boundary (other local users, browsers,
  DNS rebinding). → A loopback service drops a connection on the first HTTP
  line and on the first line that isn't its protocol, never defaults to
  unauthenticated (generate a CSPRNG token and publish it in a user-only
  file, as Jupyter does), and has a test replaying a browser-style request.

- **Fuzz every payload variant, not just one.** R17's KTX2 fuzz test first
  mutated only UASTC files; the review asked for ETC1S too, and the first
  ETC1S run crashed inside libktx (it ignores `decode_palettes` failures and
  transcodes with half-built codebooks; fixed in the `ktx` overlay port) and
  ASan then found a wrapped allocation from out-of-order level offsets. →
  Fuzz tests cover each codec / format branch, biased into each structure
  the parser trusts, and get one run under ASan + valgrind before merging.
- **libktx's `ktx` and `ktx_read` archives cannot share a program** (same
  objects, duplicate symbols). engine_asset compiles against the headers and
  each executable links one: the game `KTX::ktx_read`, tests / cooker
  `KTX::ktx`.
- **meshoptimizer 1.x encodes vertex codec v1 by default, which
  `EXT_meshopt_compression` does not allow** (v1 is for the newer
  `KHR_meshopt_compression`). The cooker calls
  `meshopt_encodeVertexBufferLevel(..., version 0)` explicitly (R17).
- **Basis UASTC -> BC5 reads R and A, not R and G.** BC5 is only correct for
  normal maps stored X-in-RGB / Y-in-alpha (libktx `normalMap`); RGB normal
  maps go to BC7, and non-BC5 targets of the X/Y layout get an RA view
  swizzle (R17).
- **libktx initialises the Basis transcoder behind an unguarded static
  flag.** Parallel transcodes are serialised until the first one succeeds
  (`ktx2_transcode.cpp`); the encoder has the same pattern, so the cooker
  encodes one image at a time with libktx's own worker threads.

- **Shadow acne has more than one cause; test it with an image test first.**
  R12.9: D16 over a caster-padded range gave 0.83 of the unshadowed value on
  a sloped lit floor; pancaking (depth clamp, tight range) alone still gave
  0.84. Per-tap receiver-plane depth plus a one-texel slope-scaled bias fixed
  it (≥ 0.97). Wide PCF / PCSS kernels need the receiver plane, not just a
  constant bias.

- **A graphics pipeline with no descriptor sets cannot draw after a pass
  that bound descriptor buffers** (validation VUID-vkCmdDrawIndexed-None-08117,
  R12): the bound buffers stay on the bind point and the pipeline lacks the
  descriptor-buffer bit. Give such a pipeline an empty set layout (the
  shadow caster pipeline does) — pass order in the graph is not fixed.
- **Screen-space marches self-intersect at grazing angles with nearest-texel
  depth** (R12 contact shadows): one pixel can span metres of depth. Sample
  the depth bilinearly (one GatherRed; NDC depth is affine over a plane) so a
  planar receiver is reconstructed exactly, and keep a grazing-floor test.
- **Far-field image tests need footprints of several pixels.** A probe on a
  floor 70 m away under a camera 3 m up covers ~7 m per pixel row; elongate
  occluders along the view axis and test a small window, not one pixel.
- **vcpkg's `acl` port is POSIX access-control lists, not the animation
  codec.** ACL + RTM are header-only and fetched by `cmake/FetchAcl.cmake`,
  pinned to release commits. Check a port's description before adding it.
- **A compressed clip can match every key and still drift between them.**
  glTF rotations interpolate with slerp, ACL (like most runtimes) with
  normalized lerp; on fast clips (the Khronos Fox's Walk / Run) the gap mid-key was
  orders of magnitude over the error budget, and ACL's keyframe stripping only checks the samples it
  removes. G8 resamples at a whole multiple of the key rate chosen by
  measuring the between-sample error, splits the budget half/half, keeps
  stripping off, and keeps a clip raw when compression does not pay.

- **slangc 2025.6.1 segfaults after reporting a compile error** when
  `-reflection-json` is passed (exit by SIGSEGV, diagnostics already
  printed). Treat any non-zero exit *or* signal as a failed compile and keep
  the captured output as the diagnostics (the R2 compiler service does).
- **A compute pipeline whose shader declares a binding its layout lacks can
  crash lavapipe inside vkCreateComputePipelines** (no validation layer in
  the container to catch it first). Hot reload therefore checks reflected
  bindings against the original layout before rebuilding a pipeline.

- **A GPU port that must match a CPU reference: never divide on the GPU.**
  Vulkan's division and sqrt are not correctly rounded and drivers fuse
  multiply-adds; compute quotients / reciprocals once on the CPU, feed both
  sides the same constants, and mirror the operation order (glm's
  `mix` is `x*(1-a)+y*a`). For bit-identical seams derive positions from
  integer lattice indices (int->float conversion and one multiply are
  exact / correctly rounded). R10.2's terrain noise lands within 1 ULP.
- **Async GPU readback without owning the submit:** a transfer->host
  barrier, then `vkCmdSetEvent2`; the host polls `vkGetEventStatus` and
  invalidates the mapped range (TerrainSynthPass). No frame counting.
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

## Scheduled fresh sessions start without repository access (cycle 1)

The first scheduled cycle (fresh session per run) cloned the public repo
read-only and stopped: pushes were refused ("not in this session's authorized
repository set") and the GitHub API was blocked. Scheduled tasks don't carry
the creating session's repository grant.
**Rule:** bind recurring cycles to the live session (it already has push + PR
access), or attach the repository to the environment before relying on
fresh-session runs. Check the first scheduled run's transcript, never assume.

## Prose was embedded as Rust string literals (A4, caught by the owner)

The A4 worker put the MCP server instructions, every tool description (some
with JSON examples), the component reference and two long prompt bodies into
`server.rs` as escaped string literals and `concat!` macros — 836 lines,
unreadable and uneditable as documentation. The brief said "idiomatic Rust"
but not where text belongs, the orchestrator's review checked behaviour and
tests, not structure, and the owner had to point it out.
**Rule:** text for models or people lives in Markdown (`content/`), embedded
with `include_str!`; code holds only error messages (CLAUDE.md "No prose in
source"). Every worker brief that adds tools, prompts or help text states this,
and merge review includes a structure pass (file sizes, inline prose, lint
level), not just a compile + test pass.

## Merge edits to quat-mcp content broke clippy (cycle 2)

Resolving merges, the orchestrator added component docs to
`tools/quat-mcp/content/components.md` and ran only `cargo test`. The content
files are attached as `#[doc = include_str!]`, so clippy's pedantic
`doc_markdown` lint checks them, and CI's Rust job failed.
**Rule:** any edit under `tools/quat-mcp/` — `content/` included — is
followed by the full trio `cargo fmt --check && cargo clippy --locked
--all-targets -- -D warnings && cargo test --locked` before pushing.
(`doc_markdown` is now allowed on the tool modules: their docs are model
text, not rustdoc — but the trio still applies.)

## A merge was committed before its compile check finished (cycle 2)

To unblock the owner, the S3.3 merge was committed and pushed while its
compile check ran in the background; the check then found merge damage
(`skinning_pass.cpp` lost the include its new `glm::mat4` use needed).
**Rule:** a merge commit is pushed only after its compile check has finished
clean. Run the check in the background if it is slow, and push when it
reports — never before.

## Docs-only pushes kept cancelling CI (cycle 2)

`concurrency: cancel-in-progress` cancels the running build on every push,
and several STATUS/ROADMAP-only pushes in a row meant no build of the merged
code finished for ~40 minutes. **Rule:** batch documentation commits with the
next code push; push docs alone only when no build of new code is running.

## CI kept getting cancelled before finishing (cycle 2)

With `concurrency: cancel-in-progress: true`, every push to the cycle branch
killed the running build; merges, docs and the hook-required pushes came
often enough that no Linux build of the merged code finished for over an
hour. **Rule:** CI runs with `cancel-in-progress: false` — a running build
always completes and GitHub keeps only the newest pending run, so each push
is eventually verified without starvation.

## Config keys added without checking for existing ones (cycle 2)

`timeout-minutes` was added to CI jobs that already had it, producing
duplicate YAML keys; a combined `git revert -q ...; git commit --amend` then
amended the already-pushed commit instead of reverting. **Rule:** grep for a
key before adding it; never chain revert + amend — make one plain new commit
and check `git status -sb` before pushing.

## Issue label update dropped the other labels (factory)

Setting `labels: ["in-review"]` through the GitHub issue update replaced the
whole label set on #16, stripping `factory`/`visual`/severity/area. **Rule:**
a label update sends the complete set — read the issue's labels first and
send them back with the status label swapped (`triaged` → `in-progress` →
`in-review`).

## Docs pushes restart the cycle PR's CI gate (factory)

A STATUS-only commit pushed to the cycle branch while its CI was queued moved
PR #3's merge gate to the new head. **Rule:** while a cycle PR waits on CI,
keep docs edits local and push them together with the next code change or
right after the merge.

## A temporary worktree moved another worktree's branch (factory)

To merge the base into a worker's PR branch, a scratch worktree ran
`git checkout -B feat/issues-4-5`; that branch was still checked out in the
finished worker's worktree, whose files then no longer matched its HEAD.
**Rule:** in scratch worktrees use a unique local branch name and push with
`git push origin HEAD:feat/<id>`; never create or reset a local branch that
another worktree has checked out (`git worktree list` shows them).

## Central registration lists turn every merge into N conflicts (factory)

Merging A8 (#17) made #19, #22, #25, #26 and #34 conflict at once — mostly
in the same lists every feature appends to (CMake source lists,
`component_json.cpp`'s component table, `remote_bridge`, quat-mcp
`server/mod.rs`, `inspector.cpp`). **Rule:** a feature adds its own files
and registers itself in one line; central lists are split per module (S9).
Until S9 lands, merge PRs that touch registration lists one at a time and
re-run CI on each merged head.

## Hand-compiled translation units are not proof of a CMake target (G8)

G8 compiled every new `.cpp` with `check-compile.sh`, but `compress_clips`'s
source was not in the target `engine_scene` links, so `game` failed to link
in CI. **Rule:** before pushing, grep the CMake lists for every new `.cpp`;
compile checks cover syntax, only a real configure + link covers targets.

## Re-merging master into every open PR burned the Actions budget (factory)

After each merge the orchestrator merged `master` into every other open PR,
including ones without conflicts. Each push started a full Linux + Rust
build, plus Windows and macOS once #24's jobs existed, and docs pushes ran
the same build. On 2026-11-27, 107 runs used about 2,700 billed minutes. The
private repo's Actions spending limit was hit at 15:52 UTC, after which
GitHub refused every new job. **Rule:** merge `master` into a PR only when
GitHub reports it unmergeable; `pull_request` CI already builds the PR merged
with the current base. Docs-only changes skip CI (#57). When a check fails
within seconds, read the check run's annotations before the logs: a billing
refusal leaves no log at all (the log download returns 404).

## A local trial merge said "clean" while GitHub said "conflict" (factory)

`git merge-tree` run from the orchestrator's checkout applied that
checkout's `.gitattributes` (`docs/LEARNINGS.md merge=union`). #24's branch
had no such line and GitHub's mergeability check ignores it, so #24 showed
clean locally and conflicting on GitHub. **Rule:** trust GitHub's
`mergeable` flag, or trial-merge in a worktree on the PR's own branch.

## Every PR needs a reviewer session, created when it leaves draft (factory)

#20 sat ready for review with no Reviewer session, so nobody looked at it
until the orchestrator noticed. **Rule:** marking a PR ready and creating
its Reviewer session are one step; the orchestrator's status sweep checks
that every non-draft PR has one.

## A hand-tuned local define compiled what CI could not (E8)

The local editor build passed `-DQUAT_PROJECT_ROOT` to every translation
unit, while CMake defines it only for the `game` target and `engine_tests`.
`inspector_world.cpp` (in `engine_editor`) used the macro, compiled locally
and failed in CI with an undeclared identifier. **Rule:** build with CI's
exact preset and defines, not a hand-tuned local set; per-target
definitions in the scratch build must mirror the CMake targets. Library
code takes the project root at runtime (from `EditorLayer::create`), never
from a compile-time path.

## Review found state bugs a smoke test cannot (E8)

The first E8 review found Reset removing and re-adding components (which
cascaded through the codec's removal rules and deleted an Animator), Remove
bypassing the codec, a recent-files loop passing a reference into the vector
it reordered, and an unvalidated ini file. Each lived in code only reachable
through an ImGui frame. **Rule:** UI actions are plain functions over the
registry or the state struct, called by the widget and by GoogleTest;
anything read from disk goes through a pure, tested parser.
