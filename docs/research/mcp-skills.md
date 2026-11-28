# Skills over MCP, Agent Skills and Claude Code plugins (A12)

Checked 2026-11-27 for ROADMAP A12, before building it. The cloud session
could not reach modelcontextprotocol.io or agentskills.io directly (blocked
by its network policy), so the findings come from the source repositories on
GitHub, read at the commits named below, and from rmcp's published crate.

## MCP skills extension (SEP-2640): final, Beta stage

- **Status.** [SEP-2640](https://modelcontextprotocol.io/seps/2640-skills-extension)
  is **Final** ([PR #2640](https://github.com/modelcontextprotocol/modelcontextprotocol/pull/2640),
  merged 2026-09-13). The extension itself is an official MCP extension at
  the **Beta** stage of the
  [extension lifecycle](https://modelcontextprotocol.io/seps/3392-extension-lifecycle):
  a released revision, but later revisions may still break it. Source of
  truth: [`modelcontextprotocol/ext-skills`](https://github.com/modelcontextprotocol/ext-skills),
  `specification/beta/skills.mdx` (read at `ba23984`, 2026-11-27); overview at
  [modelcontextprotocol.io/extensions/skills/overview](https://modelcontextprotocol.io/extensions/skills/overview).
- **Base protocol.** Written against MCP **2026-07-28** (the stateless
  revision: `server/discover`, per-request `_meta` with protocol version and
  client capabilities). Extensions are declared under `capabilities.extensions`
  with reverse-DNS ids.
- **Wire format.**
  - Capability: `"extensions": {"io.modelcontextprotocol/skills": {}}`
    (optional setting `directoryRead: true`), and the server **must** also
    declare `resources`.
  - Every file of a skill is a resource, conventionally
    `skill://<skill-path>/<file>`; the last path segment equals the skill's
    frontmatter `name`. Files are read with plain `resources/read`.
  - `skills/list` (paginated, `ttlMs` + `cacheScope` required) and
    `skills/get {uri}` return `Skill` entries: `uri` of the `SKILL.md`,
    `frontmatter` (the YAML verbatim as JSON) and `resources`, a complete
    manifest of every file with `sha256:` digest and byte size (or
    `"dynamic"`). Unknown skill URI: error `-32602`.
  - Optional `resources/directory/read` lists a directory's children (only
    with `directoryRead`).
  - Limits per skill: 512 files, 16 MiB.
  - Clients do not declare support; to a client without the extension the
    `skill://` resources are ordinary resources. Servers may point at skill
    URIs from their `instructions`.
- **Hosts.** Claude Code 2.1.332 (2026-11-27) added "skills from MCP servers
  (beta)", shown as `server:skill` in the `/` menu
  ([CHANGELOG](https://github.com/anthropics/claude-code/blob/main/CHANGELOG.md)).
  The [client matrix](https://modelcontextprotocol.io/extensions/client-matrix)
  lists the others.

## Agent Skills format

[agentskills.io/specification](https://agentskills.io/specification)
(source: [`agentskills/agentskills`](https://github.com/agentskills/agentskills)
`docs/specification.mdx`, 2026-11-04):

- A skill is a directory with a `SKILL.md`: YAML frontmatter, then Markdown
  instructions. Optional `scripts/`, `references/`, `assets/`.
- Frontmatter: `name` (required; 1-64 of `a-z0-9-`, no leading, trailing or
  doubled hyphen, equal to the directory name), `description` (required,
  1-1024 characters: what it does and when to use it), `license`,
  `compatibility` (≤ 500 characters), `metadata` (string → string map),
  `allowed-tools` (experimental).
- Progressive disclosure: name + description load at startup, the body on
  activation (keep `SKILL.md` under 500 lines / ~5000 tokens), other files on
  demand.

## Claude Code plugins and marketplaces

From the [plugin docs](https://docs.claude.com/en/docs/claude-code/plugins),
the [`anthropics/claude-plugins-official`](https://github.com/anthropics/claude-plugins-official)
examples and `claude plugin validate` (2.1.332):

- A plugin is a directory with `.claude-plugin/plugin.json` (`name`
  required; `version`, `description`, `author`, `keywords`, `userConfig`, ...)
  and default component folders at its root: `skills/<name>/SKILL.md`,
  `commands/`, `agents/`, `hooks/`, and `.mcp.json` for MCP servers
  (`{"<server>": {"command", "args", "env"}}`; `${CLAUDE_PLUGIN_ROOT}` and
  `${user_config.<key>}` are substituted).
- `userConfig` options are asked for when the plugin is enabled
  (`sensitive: true` values go to the keychain).
- Installed plugins are copied into a cache, and component paths that are
  symlinks leaving the plugin directory are refused: skills must be real
  files inside the plugin.
- A marketplace is `.claude-plugin/marketplace.json` at the repository root
  (`name`, `owner`, `plugins[]` with `name` and `source`); a `source` of
  `"./path"` points into the same repository. Users run
  `/plugin marketplace add <owner>/<repo>`, then
  `/plugin install <plugin>@<marketplace>`.
- Plugin skills are namespaced `plugin:skill`; plugin MCP servers are listed
  under "Plugin MCPs".

## rmcp (Rust SDK)

[rmcp 3.12.0](https://crates.io/crates/rmcp) (2026-11-26, the version
quat-mcp already uses; [`modelcontextprotocol/rust-sdk`](https://github.com/modelcontextprotocol/rust-sdk))
implements the extension (since 3.9, hardened in 3.10/3.11):

- model types `Skill`, `SkillFrontmatter`, `SkillResource::from_bytes`
  (digest + size), `SkillResources`, `ListSkillsResult`, `GetSkillResult`,
  `Skill::validate`;
- `ServerCapabilities::builder().enable_skills()` /
  `enable_skills_with_directory_read()`;
- `ServerHandler::list_skills`, `get_skill`, `read_resource_directory`, with
  the extension's checks (method not found unless declared, URI must name a
  `SKILL.md`, entries validated before sending);
- client `list_all_skills`, `get_skill`;
- protocol 2026-07-28 and the legacy `initialize` handshake (2025-11-25 and
  older) on the same server;
- optional `skills-fs` feature: `SkillCatalog::from_dir` serves skills from
  a directory on disk.

No upgrade was needed.

## What quat-mcp does with it

- Skills live in `tools/quat-mcp/content/skills/<name>/SKILL.md` and are
  compiled into the binary (`include_str!`), so the server needs no checkout
  at run time; `skills-fs` is not used.
- quat-mcp declares `io.modelcontextprotocol/skills` (no `directoryRead`:
  every manifest is complete, so a host never needs it), lists the
  `SKILL.md` resources in `resources/list`, answers `skills/list`,
  `skills/get` and `resources/read` for `skill://` URIs, and names the skill
  URIs in its instructions.
- The same skills are rendered as MCP prompts of the same names for clients
  without skills (template per skill in `content/prompts/`).
- The Claude Code plugin (`tools/quat-mcp/plugin/`) ships a copy of the
  skills plus a `.mcp.json` entry that runs quat-mcp with `--skills=false
  --prompts=false`, so Claude Code does not list every workflow three times.
  The repository root holds the marketplace.
