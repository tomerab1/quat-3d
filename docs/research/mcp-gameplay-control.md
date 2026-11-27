# MCP gameplay control, protocol upgrades and skills (design)

_Status: design proposal, 2026-11-27. Feeds ROADMAP items A10, A11 and A12._

The owner's request: *let the agent control behaviour and physics through
the MCP server, scripting too if possible, and use the newer MCP features,
such as servers exposing skills.*

This document does four things:

1. It audits what a model can read, edit, simulate and observe today. That
   covers `master` plus the open PRs: #22 (G7 Lua scripting), #19 (A6 shader
   authoring), #26 (R10.3 particles) and #28 (G8 animation). A9's branch
   `feat/a9-worldbuilding` had not been pushed when this audit was made, so
   A9 is taken from its ROADMAP entry.
2. It summarises the state of the Model Context Protocol (MCP) and its
   clients in late 2026. Facts carry a source; speculation is marked as such.
3. It proposes new engine remote methods and quat-mcp tools for physics,
   behaviour, scripting and play-mode control. It also shows how to adopt
   the newer protocol features in quat-mcp.
4. It plans how to ship quat-3d workflows as **skills**.

All engine-side proposals follow the existing rules in CLAUDE.md
("Remote control & MCP") and [`remote-control.md`](../remote-control.md):

- Every parameter is validated through `core::JsonFields` (never
  `nlohmann::json::get`/`value`).
- Errors are precise and name the field and what was expected.
- Paths are confined to the project root, or to a narrower directory.
- Handlers run single-threaded on the main thread from the per-frame poll.
- Slow work uses deferred replies, never a blocking handler.
- All model-facing text lives in Markdown under `tools/quat-mcp/content/`.

---

## 1. Coverage audit

Legend:

- **E**: exists on `master` (file cited).
- **P#n**: lands with open PR *n*.
- **part**: partly covered.
- **M**: missing.

"Read" means a model can see the state, "Edit" means it can author or change
persistent data, "Control" means it can drive the running simulation, and
"Observe" means it can debug or watch it over time.

Engine files are under `src/remote/world/` unless noted. MCP files are
`tools/quat-mcp/src/server/*_tools.rs`.

| System | Read | Edit | Control (runtime) | Observe / debug |
|---|---|---|---|---|
| Entities / components | **E** `scene.list/get/components` (`scene_commands.cpp`) | **E** `scene.create/update/remove_components/destroy/batch` (`scene_commands.cpp`, `batch_command.cpp`) | n/a | **E** `debug.watch` (`debug_query_commands.cpp`); **M** change feed (no "what changed since X") |
| Transforms / hierarchy | **E** `scene.get` (local TRS + `world_position`) | **E** `scene.update`, `scene.set_parent` | **part**: writing a transform while playing teleports kinematic bodies; dynamic bodies are overwritten by physics | **E** `debug.watch`; **M** world bounds (A9 plans `world.place_on_ground` and bounds) |
| Physics: bodies, colliders | **E** `collider` / `rigidbody` components (`motion`, `mass`, `shape`, `half_extents`, `sensor`) | **part**: no friction, restitution, damping, gravity factor, CCD, axis locks; box, sphere and capsule shapes only (no convex or mesh in the codec) | **M** (Lua gets `add_impulse` / `set_velocity` in **P#22**, `src/physics/physics_world.hpp`) | **part**: `debug.stats` counts bodies |
| Physics: forces, impulses, velocities | **M** (velocity readable from Lua only, **P#22**) | n/a | **M** over remote; **P#22** for Lua (impulse, linear velocity; no angular, no force) | **M** |
| Physics: gravity, world settings | **M** (hard-coded: Jolt `-9.81` in `physics_world.cpp`; characters integrate their own `-9.81` in `scene/physics_system.cpp`) | **M** | **M** | **M** |
| Physics: joints / constraints | **M** (no joint component; `PhysicsWorld` has no constraint API) | **M** | **M** | **M** |
| Physics: raycasts / overlaps / shape casts | **E** `debug.raycast` (closest hit); **P#22** `physics.raycast` in Lua | n/a | n/a | **M** overlaps, shape casts, all-hits, layer masks |
| Physics: contact events | **M** over remote (`PhysicsWorld::trigger_events()` records sensor overlaps only); **P#22** Lua `on_trigger_enter/exit` | n/a | n/a | **M** contact / trigger event feed |
| Physics: sim stepping | **E** `debug.pause/resume/step/time_scale` (`debug_commands.cpp`, `core/sim_clock.cpp`) | n/a | **E** step N frames at a fixed dt | **part**: no checkpoint/restore, no "run until"; `dt` that isn't a multiple of 1/120 s leaves accumulator remainder |
| Character controller | **E** `character` component (`radius`, `half_height`, `speed`) | **E** | **M** over remote (move direction, jump); **P#22** Lua `move`/`jump`/`on_ground` | **part** `debug.watch` (velocity, on_ground) |
| Navigation | **E** `nav_agent` component + `nav_agent_state` in `debug.watch` | **E** `nav_agent`, `patrol` | **E** set `nav_agent.target` via `scene.update`; **P#22** Lua `set_nav_target` | **E** `debug.nav_path`; **E** `nav.bake` (blocking); **M** bake progress / cancel, navmesh visualisation data |
| Behaviour trees: authoring | **E** `bt.leaves/get` (`behavior_commands.cpp`) | **E** `bt.set/add_node/remove_node/move_node/set_params` (`behavior_edit_commands.cpp`) | **part** restart-on-edit only | **E** `bt.status` (last tick per node) |
| Behaviour trees: runtime | **M** blackboard (none in the engine: `BtNodeState` holds only per-node scratch; G2 ticked but no blackboard exists in `scene/components.hpp`) | **M** | **M** set blackboard values, force a branch | **M** tick history, breakpoints, step-one-tree |
| Lua scripting | **P#22** `script.list/get` (`script_commands.cpp`) | **P#22** `script.create/update/delete/check`, `script` component | **P#22** hot reload on update; **M** call exported functions, eval, restart one instance | **P#22** errors in `debug.log` (category `script`) + `status`; **M** per-instance `self` inspection, per-script timings |
| Animation | **E** `animator` component (edit only); **P#28** clip list, clip switch, blend mode | **P#28** `animator.clip/blend/blend_time`, `foot_ik` component | **part** via component writes | **M** state-machine parameters / current state, pose or joint query |
| IK (G8) | **P#28** `foot_ik` | **P#28** | **M** IK targets (two-bone IK has no remote target API) | **M** |
| Audio | **E** `audio_source` / `audio_listener` | **E** | **E** write-only `command` play/stop/pause/resume | **M** bus volumes and mixer, playing voices |
| Particles | **P#26** `particle_emitter` component | **P#26** | **part**: `enabled` / burst fields; **M** one-shot "burst now" | **M** live particle counts / budget |
| Materials / shaders | **E** `material.get/set` (`tuning_commands.cpp`); **P#19** `shader.list/get/template` | **E** material overrides; **P#19** `shader.create/update/delete` (deferred compile) | n/a | **P#19** diagnostics in results |
| Render settings | **E** `render.settings.get/schema` | **E** `render.settings.set/reset`, `environment.set` | n/a | **E** `debug.stats` timings |
| Camera / screenshots | **E** `camera.get` | **E** `camera.set/frame` | **M** look through a game camera / follow an entity | **E** `render.screenshot` (deferred, `screenshot_capture.cpp`); A9: offscreen camera |
| Play mode / time | **E** `engine.info` (`playing`, `paused`) | n/a | **E** `sim.play/stop`, pause/step/scale; **M** checkpoints, run-until, input injection | **M** sim-state change events |
| Scene files | **E** `scene.save/load` (confined paths, `app_commands.cpp`) | **E** | n/a | **M** dirty / unsaved state |
| Input | **M** | n/a | **M** inject actions, axes or keys (**P#22** adds the input sampler that would receive them) | **M** |
| Editor | **M** selection, undo history (E1 is not built) | **M** | n/a | **M** |

### MCP surface (quat-mcp, `master`)

There are 50 tools: scene, world, sim, debug, tuning and behaviour, plus
`engine_call` as a raw escape hatch. There are five prompts (`build_world`,
`inspect_scene`, `debug_session`, `design_behavior`, `tune_look`) and one
resource (`quat://scene/summary`). PR #22 adds `script_*` tools, a
`write_game_script` prompt and a scripting reference resource. PR #19 adds
`shader_*` tools.

Protocol-wise quat-mcp uses rmcp **3.12.0** (`Cargo.lock`) with the
features `server`, `macros`, `transport-io`, `schemars` and `base64`. It
already uses:

- tool annotations on 39 of the 50 tools (`read_only_hint`, `destructive_hint`,
  `idempotent_hint`);
- server instructions;
- prompts;
- one static resource.

It does not use:

- structured output (`outputSchema` / `structuredContent`): every result is
  JSON text (`results.rs::json_result`);
- resource templates, subscriptions or completions;
- progress, cancellation forwarding or tasks;
- elicitation (the `elicitation` cargo feature is off);
- skills;
- icons.

### Gaps that matter most for "control behaviour, physics, scripting"

1. **No runtime physics control over remote.** There are no impulses,
   forces, velocities or wake/sleep. PR #22 adds impulse and linear
   velocity to `PhysicsWorld` for Lua only.
2. **No physics world settings.** Gravity is hard-coded twice: in Jolt and
   in the character integrator.
3. **No joints or constraints at all.** This covers the engine API, the
   component and the remote API, and it blocks ragdolls (G8), doors,
   vehicles and destruction (G10).
4. **No contact or trigger event feed.** A model can't see *what hit what*.
   It can only raycast after the fact.
5. **No overlap or shape-cast queries,** and no all-hits raycast or
   layer-mask filters.
6. **No behaviour-tree runtime state beyond the last tick.** There is no
   blackboard (the engine has none), no tick history and no breakpoints.
7. **No script runtime introspection.** You can't inspect an instance's
   `self`, call an exported function or eval in the sandbox. Authoring and
   hot reload arrive with #22.
8. **No checkpoint/restore or "run until".** Every experiment ("does the
   crate fall through at t = 3 s?") needs a manual stop/play and its
   timing is not repeatable.
9. **No input injection.** A model can't playtest a scripted game (#22's
   coin collector needs WASD and Space).
10. **No change feed for subscriptions.** The engine is strictly
    request/response. Apart from `debug.log` there is no cursor-based event
    stream that quat-mcp could turn into resource-updated notifications.

Secondary gaps:

- animation state-machine parameters;
- audio bus mix;
- particle "burst now";
- editor selection;
- progress and cancel for navmesh bake, cooking and shader compiles;
- undo (E1), so remote edits are not undoable.

---

## 2. MCP spec and ecosystem (late 2026)

### 2.1 Facts with sources

**Spec revisions.** Dated revisions are 2024-11-05, 2025-03-26, 2025-06-18,
2025-11-25 and **2026-07-28**. The last is the current one; the official
Claude announcement calls it "the fifth spec release"
([Claude blog](https://claude.com/blog/bringing-mcp-2026-07-28-to-claude);
[MCP blog](https://blog.modelcontextprotocol.io/posts/2026-07-28/);
[changelog](https://modelcontextprotocol.io/specification/2026-07-28/changelog)).
rmcp 3.12 knows exactly these five (`ProtocolVersion::KNOWN_VERSIONS` in
rmcp's `src/model.rs`).

2026-07-28 changed the following, per the
[MCP blog post](https://blog.modelcontextprotocol.io/posts/2026-07-28/) and
the rmcp 3.x changelog:

- **Stateless core** (SEP-2575, SEP-2567). There is no
  `initialize`/`initialized` handshake and no `Mcp-Session-Id`. Each
  request carries its protocol version, client info and capabilities in
  `_meta`. An optional `server/discover` reports capabilities.
- **Multi round-trip requests (MRTR, SEP-2322).** Server-initiated
  `elicitation/create`, `sampling/createMessage` and `roots/list` are
  replaced on 2026-07-28 connections. The server returns
  `resultType: "input_required"` with `inputRequests`, and the client retries
  with `inputResponses` and the echoed `requestState`. rmcp refuses
  server-to-client requests on 2026-07-28 peers ("return
  InputRequiredResult instead", `src/service/server.rs`).
- **Subscriptions.** Change notifications move to one `subscriptions/listen`
  stream. The client opts in per type: tools, prompts or resources
  list-changed, and per-URI `resourceSubscriptions`. `resources/subscribe`
  is legacy-only (rmcp `src/model.rs`, SEP-2575 listen streams).
- **Tasks extension** (SEP-2663, `io.modelcontextprotocol/tasks`). Tasks
  left the experimental core. A `tools/call` may answer
  `resultType: "task"`; the client polls `tasks/get`, answers mid-task
  questions with `tasks/update` and cancels with `tasks/cancel` (rmcp
  `src/model/task.rs`, `src/task_manager.rs`).
- **Cache hints** (SEP-2549): `ttlMs` / `cacheScope` on list and read
  results.
- **HTTP routing headers** (SEP-2243).
- **Auth hardening** (SEP-2468, SEP-837, SEP-2352; CIMD preferred over DCR).
- **Extensions framework** (SEP-1724 capability `extensions`). Named
  extensions include Tasks, MCP Apps and Enterprise-Managed Authorization.
- **Deprecated** (SEP-2577): **Roots, Sampling and Logging**, functional for
  at least 12 more months. The legacy HTTP+SSE transport is also
  deprecated.

Older features, still current:

- **Structured tool output** (`outputSchema` + `structuredContent`) since
  2025-06-18. SEP-2106 relaxed both to any JSON Schema type (rmcp 3.0).
- **Resource links and embedded resources** in tool results (2025-06-18).
- **Elicitation**: form mode since 2025-06-18; URL mode since 2025-11-25
  (SEP-1036; rmcp 0.15).
- **Tool annotations**: `title`, `readOnlyHint`, `destructiveHint`,
  `idempotentHint`, `openWorldHint`. All are hints that clients must not
  trust from untrusted servers (rmcp `src/model/tool.rs`).
- **Icons and `websiteUrl`** on server info, tools, prompts and resources
  (SEP-973, rmcp 0.6.4+).
- **Completions** (`completion/complete`) for prompt and resource-template
  arguments.
- **Progress notifications** (`notifications/progress`) and cancellation
  (`notifications/cancelled`).
- **MCP Apps**: `ui://` resources a host renders as interactive UI
  ([Claude blog](https://claude.com/blog/bringing-mcp-2026-07-28-to-claude);
  [Claude Code MCP docs](https://code.claude.com/docs/en/mcp)).

**Skills over MCP: SEP-2640, the Skills extension** (`io.modelcontextprotocol/skills`).
It is Final: the working-group charter says it merged on 2026-09-13
([SEP-2640](https://modelcontextprotocol.io/seps/2640-skills-extension);
[WG charter](https://modelcontextprotocol.io/community/skills-over-mcp/charter);
[ext-skills repo](https://github.com/modelcontextprotocol/experimental-ext-skills)).

- A skill is an Agent Skills folder served over the **Resources**
  primitive.
- `skills/list` and `skills/get` return each skill's **manifest**:
  - the `SKILL.md` URI, by convention `skill://<path>/SKILL.md`;
  - the parsed frontmatter;
  - every file with a `sha256:` digest and its size, or `"dynamic"`.
- `resources/read` serves the bytes.
- An optional `resources/directory/read` lists folders.
- Names follow the Agent Skills rules: at most 64 characters, lowercase
  letters, digits and single hyphens, matching the folder name.

All of this is from rmcp `src/model/skill.rs` and
`src/handler/server/skill_catalog.rs`. rmcp supports it from **3.9.0**
(2026-11-05, model and capability negotiation). **3.10.0** added
`SkillCatalog::from_dir` (feature `skills-fs`), and 3.11 hardened it.
FastMCP (Python) and pmcp (Rust) also ship it
([FastMCP docs](https://gofastmcp.com/docs/python-sdk/fastmcp-skills-extension)).

**Agent Skills format.** This is an open standard first published by
Anthropic ([agentskills.io](https://agentskills.io/specification)). A skill
is a folder with `SKILL.md`: YAML frontmatter (`name`, `description`
required) and a Markdown body, plus optional `scripts/`, `references/` and
`assets/`. It loads progressively: name and description first, the body
when used, and files on demand.

**rmcp** ([crate](https://crates.io/crates/rmcp),
[repo](https://github.com/modelcontextprotocol/rust-sdk)) is the official
Rust SDK; 3.12.0 was released 2026-11-26. Features relevant here:

| Feature | rmcp |
|---|---|
| Structured output (`Json<T>` return wrapper → `outputSchema` + `structuredContent`; `#[tool(output_schema = ..)]`) | since 0.4.0; non-object schemas since 3.0 |
| Annotations, icons and `_meta` on `#[tool]` | annotations 0.x; icons 0.6.4; `meta`/`icons`/`output_schema` macro args in 3.x |
| Elicitation (legacy `peer.elicit::<T>()`, typed schemas; URL mode) | 0.8.2 typed, 0.15 URL; cargo feature `elicitation` |
| MRTR `InputRequiredResult` + `InputResponses` / `RequestState` extractors | 3.0 (beta.1), handler access 3.1.1; sealed state with feature `request-state` |
| Resource subscriptions: legacy `enable_resources_subscribe` + `notify_resource_updated`; `subscriptions/listen` `SubscriptionSink` | legacy since 0.x; listen since 3.0 |
| Tasks: `TaskManager` (spawn, `tasks/get`/`update`/`cancel`, TTL), `ServerCapabilities::enable_tasks()` | SEP-1686 since 0.13; SEP-2663 extension since 3.0 |
| Progress / cancellation (`RequestContext.ct`, `peer.notify_progress`) | since 0.x; abandoned-call cancel (client side) 3.6 |
| Completions | since 0.x |
| Skills extension: `enable_skills()`, `list_skills` / `get_skill`, `SkillCatalog` | 3.9 (model), 3.10 (`skills-fs`) |
| Roots, sampling, logging | deprecated since 1.8 (SEP-2577) |

### 2.2 What the clients support today

**Claude Code** ([MCP docs](https://code.claude.com/docs/en/mcp),
[skills docs](https://code.claude.com/docs/en/skills),
[plugins reference](https://code.claude.com/docs/en/plugins-reference)):

- **Protocol 2026-07-28** through its v2 MCP runtime, the default since
  v2.1.274.
- **Tasks** from v2.1.304 in the terminal CLI, v2.1.331 in VS Code and the
  desktop app. Tasks need the server on 2026-07-28. `/tasks` shows the
  latest reported progress, and `tasks/cancel` is sent when the user stops
  a task.
- **Elicitation**: form and URL modes, declared as
  `elicitation: {form:{}, url:{}}` for user-configured stdio servers
  (v2.1.287+) and on 2026-07-28. Elicitation hooks can auto-answer, and
  cloud sessions show answer cards.
- **Progress notifications** reset the idle timeout, which is 30 minutes
  for stdio, but not the hard wall-clock timeout.
- **Annotations**: `readOnlyHint` lets calls run in parallel with other
  read-only calls; `idempotentHint` and `readOnlyHint` allow one retry
  after a dropped connection (HTTP servers). `_meta["anthropic/maxResultSizeChars"]`
  raises the 50 000-character persist-to-file threshold (up to 500 000).
  `_meta["anthropic/requiresUserInteraction"]` forces a full permission
  prompt.
- **Resources**: `@server:uri` mentions, plus built-in list and read
  resource tools for the model.
- **Prompts** become `/server:prompt` commands.
- **`list_changed`** refetches tools in interactive sessions.
- **MCP skills** (SEP-2640) are in **public beta** from v2.1.331, shown as
  `<server>:<skill>`. They need feature-flag fetching and a recent model.
  They are on for Pro, Max and Ultra logins, and off for Team and
  Enterprise until an admin enables them. With an API key they are still
  rolling out. Claude Code ignores `allowed-tools`, `hooks` and `paths`
  frontmatter on MCP skills, and permission rules use
  `Skill(mcp__<server>__*)`.
- **Repository skills**: `.claude/skills/<name>/SKILL.md` (project, walked
  up to the repo root, nested directories on demand), `~/.claude/skills`
  (personal) and plugin `skills/`.
- **Plugins** bundle skills, MCP servers (`.mcp.json` or `mcpServers` in
  `.claude-plugin/plugin.json`, with `${CLAUDE_PLUGIN_ROOT}`), hooks and
  agents. A plugin is installed from a marketplace (`/plugin marketplace
  add <repo>`) or tried with `--plugin-dir`.

The Claude Code docs say nothing about **structured output** or **resource
subscriptions** (`subscriptions/listen` / `resources/subscribe`).

**Claude Desktop / claude.ai.**

- Tools, prompts and resources work for local servers
  ([support article](https://support.claude.com/en/articles/10949351-getting-started-with-local-mcp-servers-on-claude-desktop)).
- MCP Apps render
  ([Claude blog](https://claude.com/blog/bringing-mcp-2026-07-28-to-claude)).
- **Elicitation is not supported** in the Desktop chat according to open
  feature requests. An August 2026 bug report says the embedded runtime
  declares the capability but never shows the prompt
  ([#41110](https://claudeissues.com/issue/41110-feature-mcp-elicitation-support-in-claude-desktop-app),
  [#88901](https://claudeissues.com/issue/88901-bug-embedded-claude-code-claude-desktop-cowork-declares-elicitation-capability-b)).
- Claude Code running inside Desktop (the Code tab) has the Claude Code
  features above.

**Other clients** support MCP skills through FastMCP-based hosts. Coverage
in VS Code / Copilot, Cursor and similar clients varies, and this audit has
no authoritative per-feature data on them.

### 2.3 Speculation, marked as such

- *Speculation:* Claude Code will probably pass `structuredContent` to the
  model, or at least keep working when it is present. The spec tells
  servers to also send the JSON as text, so emitting both is safe
  whatever the client does.
- *Speculation:* resource subscriptions (`subscriptions/listen`) have no
  documented consumer in Claude Code today. Treat them as cheap to offer
  and never as the only way to get data.
- *Speculation:* MCP skills will leave beta in Claude Code and spread to
  Desktop chat. Until they do, quat-3d skills need a second delivery path
  (a Claude Code plugin; see §4).
- *Unverified:* modelcontextprotocol.io and agentskills.io could not be
  fetched from this session (blocked by the network allowlist). Facts
  about them come from search summaries, the MCP blog, and rmcp's own
  source and changelog, which implement the spec.

---

## 3. Design proposal

### 3.1 Engine-side foundations (needed by several groups)

**F1. Engine event ring (`events.*`).** This is one bounded ring
(capacity 8192, like the log ring in `engine/core/log.hpp`) of typed
events with a sequence number. Producers push from the main thread only.
Physics contacts are collected inside Jolt's listener under the existing
`trigger_mutex` pattern and drained into the ring after `update()`, so no
new hot-path locks are added. Event kinds:

- `contact_begin` / `contact_end`: entity pair, point, normal, impulse;
- `trigger_enter` / `trigger_exit`;
- `script_error`;
- `bt_breakpoint`;
- `sim_state` (play, stop, pause, step done);
- `scene_changed` (create, destroy or update, with entity ids, coalesced
  per frame);
- `selection_changed` (editor);
- `nav_agent` (arrived or failed);
- `job_progress`.

| Method | Params | Result |
|---|---|---|
| `events.poll` | `{since_seq?, kinds?:[...], entities?:[...] (≤64), limit?=200 (1..2000)}` | `{events:[{seq, frame, sim_time_s, kind, ...}], cursor, more, missed, oldest_seq}` |
| `events.kinds` | — | `[{kind, fields, help}]` |

This uses the same cursor semantics as `debug.log` (`missed`, and
restarting from the oldest line after an engine restart). Contact events
are high-volume, so they are recorded only while at least one poll
mentioned `contact_*` in the last 10 s, or `physics.events.enable` is on.
That keeps the cost zero when nobody is listening.

This is the change feed quat-mcp needs for subscriptions (§3.3). Polling
keeps the engine request/response and single-threaded. An engine-to-client
push notification can come later without changing the event schema.

**F2. Engine jobs (`jobs.*`).** Long engine work (navmesh bake, shader
compiles, cooking, terrain synthesis, A9 asset fetch) registers a job with
`{id, kind, progress 0..1, message, cancellable}`. The worker writes an
atomic progress value; the main thread publishes it in `jobs.get`.

| Method | Params | Result |
|---|---|---|
| `jobs.list` | — | `{jobs:[{id, kind, state, progress, message, started_s}]}` |
| `jobs.get` | `{id}` | one job |
| `jobs.cancel` | `{id}` | `{id, state}`; `failed` if not cancellable |

A deferred method that starts a job answers immediately with `{job: id}`
when called with `async: true`. Otherwise it replies when done, as today.
quat-mcp maps jobs to MCP tasks and progress (§3.3).

**F3. Simulation checkpoints.** PR #22 moves `scene_snapshot` into `scene/`.
Build on it:

| Method | Params | Result |
|---|---|---|
| `sim.checkpoint.save` | `{name}` (`[a-z0-9_-]{1,32}`, ≤ 8 kept) | `{name, frame, sim_time_s, entities}` |
| `sim.checkpoint.restore` *(deferred, 1 frame)* | `{name, paused?=true}` | `<clock>` |
| `sim.checkpoint.list` / `.drop` | `{name}` | names with metadata |

A checkpoint holds three things:

- the ECS snapshot (codec JSON);
- per-body Jolt state: position, rotation, linear and angular velocity,
  and sleeping state, captured with `JPH::StateRecorder` (`SaveState` /
  `RestoreState`, the standard Jolt rollback API);
- script instance properties. Lua `self` tables are not serialised;
  scripts get `on_destroy` / `on_start` on restore, which is documented.

Restore runs at the start of a frame before systems tick. It is only valid
while playing; edit mode already has `scene.save/load`.

**Determinism contract.** It is documented in remote-control.md and
enforced:

- `debug.step` accepts `dt` that is a multiple of `1/120` s exactly
  (`substeps` alternative param), so the physics accumulator carries no
  remainder.
- Jolt runs with a fixed job count while stepping.
- Given the same checkpoint and the same injected input,
  `restore → step N` reproduces the same body states on the same binary
  and platform. Jolt is deterministic in that case; cross-platform needs
  `JPH_CROSS_PLATFORM_DETERMINISTIC`, which is out of scope here.
- A GoogleTest proves it: restore, step 240, hash body states, repeat.

### 3.2 New remote methods and MCP tools

Each MCP tool maps 1:1 to an engine method. Its description lives in
`content/tools/<tool>.md`, its params in `src/params/<group>.rs`, and the
tool group in `src/server/<group>_tools.rs`.

#### Physics control (A10.1)

Physics state exists while playing (or in the demo modes). Otherwise every
`physics.*` call except `settings.*` is `unavailable`, with the hint "call
sim_play". An entity reference resolves to its `RigidBody.body` or
`CharacterController.character`. An entity without one is `invalid_params`
("entity 12 has no rigidbody; add collider + rigidbody").

| Method | Params | Result / notes |
|---|---|---|
| `physics.body.get` | `{entity}` or `{entities:[...]}` (≤64) | `{id, motion, mass, linear_velocity, angular_velocity_deg, awake, gravity_factor, friction, restitution, linear_damping, angular_damping, ccd, locked_axes, layer, sensor}` |
| `physics.body.set` | `{entity, linear_velocity?, angular_velocity_deg?, awake?}` | runtime state only; dynamic bodies only (`invalid_params` for static/kinematic, naming the motion); finite, \|v\| ≤ 1e4 m/s |
| `physics.apply` | `{entity, impulse?, angular_impulse?, force?, torque?, at?:[x,y,z] (world point), space?: "world"\|"local", duration_s?}` | exactly one of the four kinds; `force`/`torque` are applied every substep for `duration_s` (default one frame; ≤ 10 s; kept in a per-body list drained by the physics system, since Jolt clears forces each step); impulses wake the body; magnitudes finite and ≤ 1e7 |
| `physics.settings.get/set` | `{gravity?:[x,y,z] (\|g\| ≤ 1000), collision_steps?: 1..8, max_substeps_per_frame?: 1..32}` | applies to Jolt **and** the character integrator (one source: replace the two hard-coded `-9.81`); saved in the scene file's optional `physics_settings` block (versioned, lenient on load like `render_settings`) |
| `physics.overlap` | `{shape:{sphere:{radius}}\|{box:{half_extents}}\|{capsule:{radius, half_height}}, position, rotation?, include_sensors?, layers?:[...], limit?=64}` | `{hits:[{entity, name, kind, layer}]}` (narrow-phase `CollideShape`) |
| `physics.shape_cast` | `{shape, from, to, rotation?, include_sensors?}` | first hit `{entity, point, normal, fraction, distance}` |
| `physics.raycast` | `debug.raycast` params + `all?: false`, `layers?` | `all` returns sorted hits (≤ 64); `debug.raycast` stays as an alias |
| `physics.contacts` | `{entity}` | the body's **current** contacts `{other, point, normal, penetration}` (from a per-body persisted-contact map kept by the listener) |
| `physics.events` | = `events.poll` restricted to `contact_*`/`trigger_*` | convenience; enables contact recording |

**Component additions** (codec, editor inspector and scene files, all via
`component_json`):

- `rigidbody`: `friction` [0, 10], `restitution` [0, 1], `linear_damping` /
  `angular_damping` [0, 100], `gravity_factor` [-10, 10], `ccd` (Jolt
  `LinearCast` motion quality), `lock_position` / `lock_rotation`
  `[bool,bool,bool]` (Jolt `EAllowedDOFs`), `start_asleep`, and read-only
  `linear_velocity` while playing.
- `collider`: `convex` (from the entity's mesh) and `mesh` (static only).
  These are listed in CLAUDE.md's shape helpers but missing from the codec.
- **New `joint` component.** It is defined on the child entity:
  - `type`: fixed, point, hinge, slider, distance, cone or six_dof;
  - `target`: entity name or id, or `null` for the world;
  - `anchor` (local) and `axis` (local; hinge and slider);
  - `limits`: `[min, max]` degrees or metres;
  - `spring`: `{frequency_hz, damping}`;
  - `motor`: `{mode: off|velocity|position, target, max_force}`;
  - `break_force` / `break_torque` (N, N·m; 0 = unbreakable), which G10
    reuses;
  - read-only `broken`.

  The physics system creates `JPH::TwoBodyConstraint`s on Play. A missing
  or bodiless target is a precise error at `scene.update` time when it can
  be known, and an `events` warning otherwise. The `PhysicsWorld`
  constraint API (create, remove, set motor target, `is_broken`) stays
  Jolt-free in the header, as the existing PIMPL does.

| Method | Params | Result |
|---|---|---|
| `physics.joint.get` | `{entity}` | `{type, target, broken, current_angle\|position, motor_state, last_lambda}` |
| `physics.joint.set` | `{entity, motor?:{mode, target}, enabled?}` | runtime-only; persistent edits go through the `joint` component |

**Character runtime control.** The model can drive a character like a
player:

| Method | Params | Result |
|---|---|---|
| `character.move` | `{entity, direction:[x,0,z] (\|d\| ≤ 1), duration_s?=0.5 (≤ 10), jump?: speed m/s}` | `{position, velocity, on_ground}`; holds the move intent for the duration (same field Lua's `e:move` writes) |

MCP tools:

- `physics_body_get` (readOnly), `physics_body_set` (idempotent);
- `physics_apply`; this is neither idempotent nor destructive, because it
  only changes the running simulation, which Stop restores;
- `physics_settings_get/set`;
- `physics_overlap`, `physics_shape_cast` and `physics_raycast` (all
  readOnly);
- `physics_contacts` (readOnly);
- `physics_events` (readOnly, cursor);
- `physics_joint_get/set`;
- `character_move`.

The `debug_session` prompt and the `debug-physics-glitch` skill (§4) teach
the loop: checkpoint, step, watch, events, then restore.

#### Behaviour runtime (A10.2)

The engine work comes first and is not just remote plumbing: a
**blackboard** for behaviour trees.

- It is a per-entity `Blackboard` component: a `std::flat_map<std::string,
  BbValue>`, where `BbValue` is a number, bool, vec3 or entity. It has at
  most 64 keys, each matching `[a-z][a-z0-9_]{0,31}`.
- It is serialised as the `blackboard` component (initial values).
- New leaves:
  - `bb_set {key, value}`;
  - conditions `bb_compare {key, op, value}`, `bb_is_set {key}` and
    `bb_near {key, range}` (distance to a vec3 or entity key);
  - `move_to {target_key}` as an alternative to x/y/z.
- `bt.leaves` reflects all of these automatically through the leaf table.
- Lua (G7) gets `e:blackboard()` get and set, so scripts and trees talk
  through one documented channel.

| Method | Params | Result |
|---|---|---|
| `bt.blackboard.get` | `{entity}` | `{values:{key: value}, initial:{...}}` |
| `bt.blackboard.set` | `{entity, values:{key: value \| null}}` | runtime values (playing) or initial values (edit, = `scene.update {blackboard}`); type changes allowed, unknown-key typos caught by listing current keys in the error when `strict: true` |
| `bt.trace` | `{entity, since_tick?, limit?=50}` | per tick: `{tick, sim_time_s, root_status, changed:[{id, path, status}]}` from a 256-tick ring per traced entity; tracing is enabled per entity by the first call (≤ 16 entities) and costs nothing otherwise |
| `bt.breakpoint.set` | `{entity, node, on: "enter"\|"success"\|"failure"\|"running", once?=true}` | `{id}`; when hit, the sim clock pauses at the end of that frame and a `bt_breakpoint` event is recorded (≤ 32 breakpoints) |
| `bt.breakpoint.list` / `.clear` | `{id?}` | |
| `bt.force` | `{entity, node, status: "success"\|"failure"}` | forces a leaf's next result once (testing a branch without staging the world) |

Stepping reuses `debug.step` (one frame equals one tick for every tree, so
nothing desyncs). `debug.watch` gains `bt_state`: the running node path
and the blackboard.

MCP tools:

- `bt_blackboard_get` (readOnly), `bt_blackboard_set` (idempotent);
- `bt_trace` (readOnly);
- `bt_breakpoint_set/list/clear`;
- `bt_force`.

The descriptions explain that breakpoints pause the whole simulation.

#### Scripting runtime (A10.3, after #22 merges)

This builds on #22's runtime (`src/script/`). Every call that touches a
running instance requires Play; otherwise it is `unavailable`.

| Method | Params | Result |
|---|---|---|
| `script.instances` | `{path?, entity?}` | `[{entity, name, path, state: running\|errored\|stopped, error?:{message, line, traceback}, properties, stats:{calls, avg_us, max_us, instructions_max}}]` |
| `script.inspect` | `{entity, depth?=2 (1..5), keys?:[...]}` | the instance's `self` as JSON (numbers, strings, bools, `vec3`→`[x,y,z]`, entities→`{entity:id}`, functions→`"<function>"`, cycles→`"<cycle>"`, ≤ 256 KiB, deeper levels `"<table: n keys>"`); read-only |
| `script.call` | `{entity, function, args?:[...] (≤ 16, JSON → Lua)}` | `{returns:[...]}`; only functions the script lists in `exports = {"collect", ...}` (explicit opt-in, so internal callbacks stay private); runs under the per-callback instruction and memory limits; Lua errors are `failed` with message and line |
| `script.eval` | `{source (≤ 16 KiB), entity?, limit_instructions?=1e6 (≤ 5e6)}` | `{returns:[...], output:[lines]}`; compiled as a **text** chunk in a fresh environment whose `__index` is the read-only script API (`scene`, `physics`, `input`, `time`, `log`, `vec3`, `quat`); `self` bound when `entity` is given; `print` captured into `output` and also logged (category `script.eval`); same sandbox as scripts (no `io/os/load/debug`), same `__gc` and `rawset` rules; gated by `QUAT_SCRIPT_EVAL` (default on in editor builds, off in the non-editor game) |
| `script.restart` | `{entity? \| path?}` | restarts errored or stopped instances (`on_destroy` + fresh `on_start`) |
| `script.api` | — | machine-readable binding reflection `[{table, name, signature, help}]` generated from the binding registration, so `docs/scripting.md`'s tables are checked against it by a test |

Logs stay in `debug.log`. Log lines gain an optional structured
`fields:{entity, path, line}`, and `debug.log` adds `entity?` / `path?`
filters, so one reader serves everything. A `quat://log/script`
resources come in A11.

MCP tools:

- `script_instances` (readOnly), `script_inspect` (readOnly);
- `script_call`, `script_eval`; `destructiveHint: false`, because their
  changes live only in the play session, which Stop reverts;
- `script_restart`;
- `script_api` (readOnly).

`write_game_script` (prompt or skill) teaches:

1. write the script;
2. `script_check`;
3. attach it;
4. `sim_play`;
5. `input_inject` to playtest;
6. `script_instances` / `debug_log` on error;
7. `script_update` to hot reload;
8. `script_eval` to probe state.

#### Play mode and time control (A10.4)

| Method | Params | Result |
|---|---|---|
| `sim.state` | — | `{mode: edit\|playing, paused, frame, sim_time_s, time_scale, checkpoints:[names], dirty}` |
| `sim.run_until` *(deferred)* | `{until: {event: <kind>, entity?} \| {watch: {entity, component, field, op: "<"\|">"\|"=="\|"!=", value}} \| {log: {contains, level?}}, max_sim_s (≤ 600), max_frames?, dt? (k/120), real_time?=false}` | `{met: bool, frame, sim_time_s, event?}`; steps as fast as allowed (fixed dt, unthrottled), pauses when met or at the limit; one active run at a time; `jobs` entry so it is cancellable |
| `input.inject` | `{actions?:{jump:true,...}, axes?:{move_x:-1..1,...}, keys?:[names], mouse?:{dx,dy,buttons}, duration_frames (1..3600)}` | `{until_frame}`; merged into the G7 input sampler after real input (injected values win), visible to Lua `input.*` and the player controller; cleared on Stop; names validated against the sampler's tables (errors list valid names) |
| `input.state` | — | current sampled actions, axes and keys (real + injected) |
| `camera.view` | `{mode: "editor"\|"game"\|"entity", entity?}` | look through the game camera or follow an entity (screenshots of gameplay) |

`sim.play` gains an optional `{checkpoint: name}` to start from a
checkpoint.

MCP tools:

- `sim_state` (readOnly);
- `sim_run_until` (task-capable, §3.3);
- `sim_checkpoint_save/restore/list/drop`;
- `input_inject`, `input_state`;
- `camera_view`.

#### Smaller follow-ups (part of A10.5, size S each)

- Animation: `animation.params.get/set {entity, params:{name: value}}` and
  `animation.state` for the G8 state machine.
- Audio: `audio.mix.get/set` for bus volumes.
- Particles: `particles.burst {entity, count}`.
- Editor: `editor.selection.get/set`.
- Each follows its owning PR's codec.

### 3.3 Using the newer MCP features in quat-mcp (A11)

quat-mcp talks stdio to one client. rmcp 3.12 answers both legacy
(`initialize`) clients and 2026-07-28 stateless clients. Every feature
below has to work on both, and must not break when a client lacks it.

| Feature | Use in quat-mcp | rmcp | Client support (2026-11) |
|---|---|---|---|
| **Structured output** | Typed result structs (`serde` + `schemars`) for stable tools: `engine_info`, `scene_get`, `scene_list`, `debug_stats`, `debug_watch`, `bt_status`, `physics_body_get`, `sim_state`, `jobs`. Returned through `Json<T>` so `outputSchema` is generated; text content keeps the same JSON. Unknown or newer engine fields are kept via `#[serde(flatten)] extra`, so a newer engine never fails a tool. A test asserts every `outputSchema` validates the mock engine's replies. | ≥ 0.4; non-object 3.0 | Claude Code: undocumented. Harmless with the text copy. |
| **Annotations** | Audit every tool: `read_only_hint` on all get/list/status/watch/log/raycast/screenshot tools; `destructive_hint` on `scene_clear`, `scene_destroy`, `scene_load`, `scene_remove_components`, `script_delete`, `shader_delete`, `render_settings_reset`; `idempotent_hint` on set-style tools; `open_world_hint: false` everywhere; `title` on all. `_meta["anthropic/maxResultSizeChars"]` (e.g. 200 000) on `scene_list`, `bt_get`, `debug_log`, `script_get`, `shader_get`. A unit test enforces a rule table (name prefix → required hints). | yes | Claude Code: parallel read-only calls, retry on reconnect (HTTP), large-result threshold. |
| **Resources + templates** | Static: `quat://scene/summary` (exists), `quat://sim/state`, `quat://editor/selection`, `quat://log/recent`, `quat://log/errors`, `quat://docs/components`, `quat://docs/scripting`, `quat://docs/behavior-leaves` (from `bt.leaves`). Templates: `quat://entity/{entity}`, `quat://script/{path}`, `quat://shader/{name}`, `quat://bt/{entity}`, `quat://physics/body/{entity}`. Tool results add `resource_link`s (e.g. `scene_create` links `quat://entity/42`). | yes | Claude Code: `@quat-3d:quat://...` mentions and model-side list/read tools; Desktop: resources supported. |
| **Subscriptions** | Subscribable: `quat://log/errors`, `quat://sim/state`, `quat://editor/selection`, `quat://scene/summary`, `quat://entity/{id}`. quat-mcp runs one poller task while ≥ 1 subscription is open: `events.poll` every 250 ms (with kinds and entities from the open subscriptions); maps events to URIs; sends `notifications/resources/updated` (legacy `resources/subscribe`) or through the `subscriptions/listen` `SubscriptionSink` (2026-07-28). No subscription means no polling. | legacy 0.x; listen 3.0 | Undocumented in Claude Code; **nothing depends on it**. The same data stays reachable by tools with cursors. |
| **Tasks** (SEP-2663) | Task-capable tools: `nav_bake`, `scene_load`, `shader_create/update` (P#19), `sim_run_until`, A9's `asset_fetch`, R17 cook, terrain regenerate. quat-mcp overrides `call_tool` for them. When the client declares the tasks extension it calls the engine with `async:true`, gets a job id, `TaskManager::spawn`s a future that polls `jobs.get` (status message = job message, poll interval 1 s) and completes with the normal tool result; `tasks/cancel` → `jobs.cancel`. Without the extension the tool behaves as today (blocking, long timeout) and sends `notifications/progress` from the same job polling when the request carried a `progressToken`. | 3.0+ (`TaskManager`) | Claude Code ≥ 2.1.304 (CLI) on 2026-07-28; `/tasks` shows progress. |
| **Progress + cancellation** | As above for non-task calls. `RequestContext.ct` cancellation → `jobs.cancel` / drop deferred `debug.step` queues. | yes | Claude Code: progress resets the idle timeout, not the hard limit. |
| **Elicitation** | Confirm destructive operations: `scene_clear` with more than 20 entities, `scene_load` when the scene is `dirty`, `script_delete` / `shader_delete` while entities use the file, `sim_checkpoint_restore` over unsaved play edits. A form with one boolean `confirm` and a short summary (counts, names). On 2026-07-28 return `InputRequiredResult` (MRTR) and read `InputResponses` on the retry. On legacy sessions use `peer.elicit`. **Fallback** when the client declares no elicitation: no blocking. The tool takes `confirm: true`; without it, a destructive call returns a dry-run summary plus "call again with confirm: true". Decline maps to an error result "declined by the user; nothing changed". | `elicitation` feature (0.8+ typed, URL 0.15); MRTR 3.0+ | Claude Code: form + URL. Desktop chat: no (open issues). Hence the fallback. |
| **Prompts** | Keep the five workflow prompts plus `write_game_script` (P#22) as short entry points that point to the matching skill (§4), so clients without skills keep a workflow. Their text is rendered from the skill's `SKILL.md` body (one source). | yes | Claude Code: `/quat-3d:build_world` commands; Desktop: prompt picker. |
| **Completions** | `completion/complete` for template args (entity names from `scene.list`, script paths, shader names) and prompt args (`entity`). | yes | Undocumented in Claude Code; low cost. |
| **Icons / metadata** | Server `title` "quat-3d", `website_url` (repo), one SVG icon (data URI); tool-group icons optional. | 0.6.4+ | Shown by some hosts; cosmetic. |
| **Skills extension** | §4. | 3.9+ / `skills-fs` 3.10+ | Claude Code ≥ 2.1.331 (public beta, gated). |
| Roots, sampling, logging | **Not adopted**: deprecated by SEP-2577. Logs stay on stderr via `tracing`. | deprecated | — |
| MCP Apps | Later, not in A11: an interactive viewport or behaviour-graph view as a `ui://` resource. Worth a spike once the core lands. | — | claude.ai / Desktop render them. |
| Authorization | Not applicable: stdio and loopback only. The engine token stays. | — | — |

**Protocol-version testing.** quat-mcp's integration tests already drive
the server with rmcp's client. A11 runs the suite twice, pinned to
2025-11-25 and to 2026-07-28. It also checks that instructions and
capabilities reach a 2026-07-28 client through `server/discover`.

### 3.4 Validation and safety checklist (engine)

- Every new handler reads params through `JsonFields`. `finish(f)` rejects
  unknown fields and lists the known ones. Vectors must be finite.
  Magnitudes and counts have the caps listed in the tables.
- Requests that would allocate without bound are capped: `entities`
  lists ≤ 64, event `limit` ≤ 2000, inspect output ≤ 256 KiB, eval source
  ≤ 16 KiB, breakpoints ≤ 32, checkpoints ≤ 8.
- No new filesystem paths. Checkpoints are in memory. Script paths keep
  #22's `assets/scripts/` confinement.
- Main thread only. Jolt callbacks only append to the existing
  listener-side buffers. Lua runs where #22 runs it (main thread, inside
  the frame).
- Runtime mutation (`physics.*`, `script.call/eval`, `input.inject`,
  `bt.force`) requires Play and is reverted by Stop. That makes it safe to
  let a model experiment. Persistent edits keep going through components.

---

## 4. Skills: shipping quat-3d workflows

### 4.1 What a skill is here

A skill is a folder with `SKILL.md`: frontmatter `name` and `description`,
plus a Markdown body describing a workflow. It can carry `references/`
files the model reads on demand.

Skills complement the tool descriptions, which say what one tool does.
Skills say **how to combine tools to reach a goal**:

- in what order;
- what to check after each step;
- which pitfalls to avoid.

They are the natural successor to the five MCP prompts. They load only
when relevant (progressive disclosure), so they don't bloat every
session's context the way long server instructions do.

### 4.2 The initial set

| Skill | Workflow | Main tools |
|---|---|---|
| `build-level` | Inspect, block out with `scene_batch`, terrain, scatter and grass, place on ground (A9), light and sky, screenshot from several cameras, iterate, save | scene_*, environment_*, world_ground_height, camera_*, screenshot, scene_save |
| `tune-look` | Read the settings schema, change one group at a time, A/B screenshots, material overrides, save | render_settings_*, material_*, screenshot |
| `debug-physics-glitch` | Reproduce in Play, checkpoint, step, watch, contact events, raycast and overlap, fix colliders or body params, restore, verify with `sim_run_until` | sim_*, debug_*, physics_*, scene_update |
| `write-gameplay-script` | Read the scripting reference, write, check, attach, play, inject input, read logs and instances, hot reload, eval probes. `references/lua-patterns.md` (issue #30) | script_*, input_inject, debug_log |
| `design-npc-behaviour` | Leaves and kinds, blackboard design, `bt_set`, play, `bt_trace` / breakpoints, iterate | bt_*, nav_bake, debug_nav_path |
| `author-material-shader` | Template, write Slang, compile diagnostics, assign, screenshot (after #19) | shader_*, material_set, screenshot |
| `playtest` | Play with a goal, run-until checks, screenshots at key moments, a structured report of failures | sim_*, input_inject, screenshot, events |

### 4.3 One source, three delivery paths

All skills live in **`tools/quat-mcp/content/skills/<name>/`** (`SKILL.md`
+ `references/*.md`), next to the tool descriptions they rely on. The
directory then serves three paths.

1. **Over MCP (SEP-2640)**, the primary path for any client that supports
   it.
   - quat-mcp declares `ServerCapabilities::enable_skills()` and implements
     `list_skills` / `get_skill`, plus `read_resource` for
     `skill://<name>/...` URIs.
   - It needs rmcp ≥ 3.9 (3.12 is already in use) and no `skills-fs`. The
     files are embedded with `include_str!` through a generated table
     (`build.rs` walks `content/skills/` and writes `skills_table.rs`), so
     the binary stays self-contained. Manifest entries use
     `SkillResource::from_bytes`, which computes the digests.
   - A `--skills/--no-skills` flag (`QUAT_MCP_SKILLS`) switches skills off
     so they don't appear twice next to path 2.
   - In Claude Code they appear as `quat-3d:build-level` and so on.
2. **A Claude Code plugin**, the fallback for Claude Code without MCP
   skills (Team or Enterprise not enabled, API-key logins during rollout,
   feature flags off).
   - `tools/quat-mcp/.claude-plugin/plugin.json` sets `"skills":
     ["./content/skills/"]`.
   - It sets `"mcpServers": {"quat-3d": {"command":
     "${CLAUDE_PLUGIN_ROOT}/dist/quat-mcp", "env": {"QUAT_MCP_SKILLS": "0"}}}`.
     The binary comes from the P6 release archive or `cargo build`. It
     is not put in a top-level `bin/`: Claude Code adds that to the Bash
     `PATH`, and claude.ai refuses plugins that have one.
   - `userConfig` holds the port and a sensitive token.
   - A repo-root `.claude-plugin/marketplace.json` lists it, so `/plugin
     marketplace add <owner>/quat-3d` then `/plugin install quat-3d` gives
     a user the server and the skills in one step.
   - For contributors working inside this repository, a project
     `.claude/skills/` entry is **not** added. It would load the gameplay
     skills into every engine-coding session, where they are noise.
     Contributors use `--plugin-dir tools/quat-mcp` instead.
3. **MCP prompts**, for clients with neither (Claude Desktop chat today).
   The existing prompts render the same `SKILL.md` body (frontmatter
   stripped; MiniJinja for the prompt's arguments). There is one text,
   not two.

**Referencing tools.** Skill bodies name tools by their bare MCP names in
backticks (`scene_batch`), as the server instructions already do. Hosts
show them prefixed (`mcp__quat-3d__scene_batch` in Claude Code), and
models map bare names reliably when the skill says "the quat-3d server's
tools". Skills do **not** copy tool parameter docs. They say "see the
`scene_create` description" and use the `quat://docs/*` resources for long
references, such as the component and scripting references. A skill's
files must sit inside its own folder, so linking to a resource (one
source) is how a skill reaches shared reference text without copying it.
"Models map bare names reliably" is an expectation to check in the skill
evaluations below, not a measured result.

**Staying in sync.** These run as `cargo test` cases next to the existing
`every_tool_has_exactly_one_content_file`:

- every backticked snake_case identifier in a skill that starts with a
  tool-group prefix (`scene_`, `physics_`, `bt_`, `script_`, `sim_`,
  `debug_`, ...) exists in `QuatServer::tools()`;
- every `quat://` URI a skill mentions is served;
- every prompt maps to a skill;
- every skill passes rmcp's `Skill::validate()` (name rules, name matches
  folder, digests);
- `description` is ≤ 1024 characters and the body ≤ 500 lines (Agent
  Skills guidance).

Claude Code-only frontmatter (`allowed-tools`, `paths`) is not used,
because MCP skills ignore it. Any edit under `content/skills/` runs the
quat-mcp trio (`fmt`, `clippy`, `test`) as LEARNINGS requires.

**Evaluation.** Each skill gets one scripted scenario against the mock
engine in `tests/`. The test checks the tool sequence the skill prescribes
works end to end, not model behaviour. A manual eval checklist goes in
`content/skills/README.md`.

---

## 5. Roadmap proposal

| Item | Scope | Depends on | Size |
|---|---|---|---|
| **A10.1 Physics control** | Engine event ring (F1); `physics.body.get/set`, `physics.apply`, `physics.settings` (one gravity source, scene block), `physics.overlap/shape_cast/raycast(all)/contacts/events`; rigidbody codec fields (friction, restitution, damping, gravity factor, CCD, axis locks); `joint` component + Jolt constraints + `physics.joint.*`; `character.move`; MCP `physics_*` tools; tests per method | — (reuses #22's `add_impulse`/velocity if merged first; otherwise lands them) | L |
| **A10.2 Behaviour runtime** | Blackboard component + `bb_*` leaves + `move_to.target_key`; `bt.blackboard.*`, `bt.trace`, `bt.breakpoint.*`, `bt.force`; `debug.watch` `bt_state`; MCP `bt_*` tools | A7 (done) | M |
| **A10.3 Script runtime** | `script.instances/inspect/call/eval/restart/api`, `exports` table, structured log fields + `debug.log` filters, Lua blackboard access; MCP `script_*` runtime tools | **#22 (G7)** merged; A10.2 for blackboard access | M |
| **A10.4 Play-mode control** | Checkpoints (F3, Jolt `StateRecorder`), determinism contract + test, `sim.state`, `sim.run_until`, `input.inject/state`, `camera.view`, `sim.play {checkpoint}`; MCP tools | #22 (input sampler, play mode outside the editor); F1 | M–L |
| **A10.5 Small runtime gaps** | Animation params/state (after #28), audio mix, `particles.burst` (after #26), editor selection | the owning PRs | S each |
| **A11 MCP protocol upgrades** | Annotation audit + rule test + `maxResultSizeChars`; structured output for stable tools; resources + templates + resource links + completions; subscriptions over F1 (legacy + `subscriptions/listen`); engine jobs (F2) + tasks + progress + cancellation; elicitation confirms (MRTR + legacy + `confirm` fallback); icons; dual-protocol test runs | F1/F2 (from A10.1); rmcp 3.12 (in use); can start with annotations/structured output immediately | M–L |
| **A12 Skills** | `content/skills/` with the seven skills above; SEP-2640 serving (embedded table); prompts rendered from skills; Claude Code plugin + marketplace manifest; sync tests; README setup docs | A11 resources (`quat://docs/*`); each skill's tools (A10.x, #19, #22, A9) — ship `build-level` + `tune-look` first, add the rest as their tools land | M |

Suggested order:

1. A11's quick wins: annotations, structured output, resources. These are
   independent of the engine.
2. A10.1, which brings the event ring that A11's subscriptions need.
3. A10.4, then A10.2.
4. A10.3 once #22 merges.
5. A12 incrementally: the first two skills right after A11's resources,
   then one skill per finished A10 slice.
