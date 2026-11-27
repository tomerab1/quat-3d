# quat-mcp content

Everything quat-mcp says to a language model lives here as Markdown, not in
Rust string literals. The crate embeds these files at compile time with
`include_str!`, so editing one only needs a rebuild.

| Path | Used as |
|---|---|
| `instructions.md` | MCP server instructions (sent on `initialize`) |
| `components.md` | Component reference appended to the tools that take component payloads |
| `tools/<tool>.md` | The description of the MCP tool with that name |
| `resources/<resource>.md` | The description of the MCP resource with that name |
| `prompts/<prompt>.md` | Prompt template ([MiniJinja](https://docs.rs/minijinja) syntax: `{{ arg }}`, `{% if arg %}…{% endif %}`), rendered with the prompt's arguments |

Write for the model: say what the tool returns, when to use it, and give a
short JSON example where the shape is not obvious. Keep each file focused;
parameter-level documentation belongs on the parameter structs in
`src/params.rs` (their doc comments become the JSON schema).
