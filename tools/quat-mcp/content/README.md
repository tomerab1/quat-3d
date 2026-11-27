# quat-mcp content

Everything quat-mcp says to a language model lives here as Markdown, not in
Rust string literals. The crate embeds these files at compile time with
`include_str!`, so editing one only needs a rebuild.

| Path | Used as |
|---|---|
| `instructions.md` | MCP server instructions (sent on `initialize`), a MiniJinja template that lists the workflows |
| `components.md` | Component reference appended to the tools that take component payloads |
| `tools/<tool>.md` | The description of the MCP tool with that name |
| `resources/<resource>.md` | The description of the MCP resource with that name |
| `skills/<name>/SKILL.md` | A workflow as an [Agent Skill](https://agentskills.io/specification): YAML frontmatter (`name` = directory, `description` of what and when, `compatibility`, `metadata.quat-mcp-tools` = every tool the text names), then the instructions. Served over the MCP skills extension, as a prompt, and in the Claude Code plugin |
| `prompts/<name>.md` | The prompt made from skill `<name>`: YAML frontmatter with the prompt's `title` and `arguments` (`name`, `description`, `required`), then a [MiniJinja](https://docs.rs/minijinja) template (`{{ arg }}`, `{% if arg %}…{% endif %}`) that frames the request and places the skill's instructions with `{{ skill }}` |
| `assets/<document>.md` | Templates for files the server writes |

Write for the model: say what the tool returns, when to use it, and give a
short JSON example where the shape is not obvious. Keep each file focused;
parameter-level documentation belongs on the parameter structs in
`src/params.rs` (their doc comments become the JSON schema).
