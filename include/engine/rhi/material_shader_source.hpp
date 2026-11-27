#pragma once

// Static checks on an authored material shader's source text (ROADMAP A6),
// run before the text is written to disk or handed to slangc. The source
// comes from a remote client, and slangc would otherwise read any file it
// names: `#include "/etc/passwd"`, `__include "..."` or `import "../x.slang"`
// pull in arbitrary files, and the compiler's diagnostics echo their lines
// back to the caller. A material may therefore only
//   * `import <module>;` (optionally `__exported`) where <module> is a plain
//     identifier naming a shader library module (assets/shaders/lib), and
//   * contain no preprocessor directives (`#` anywhere outside comments and
//     string literals), no identifier starting with `__` other than
//     `__exported` (`__include`, `__import`, ...), no character literals
//     (a `"` inside `'"'` would open a fake string), and no backslash line
//     splicing outside string literals.
// Comments and string literals are ignored when looking for these; a string
// literal ends at its line's end at the latest, so nothing after a newline is
// ever hidden from the check.

#include <expected>
#include <span>
#include <string>
#include <string_view>

#include "engine/core/error.hpp"

namespace engine::rhi {

// Ok, or an error naming the 1-based line and what is not allowed there.
// `modules` are the importable library module names (file stems).
[[nodiscard]] std::expected<void, core::Error>
check_material_source(std::string_view text, std::span<const std::string> modules);

} // namespace engine::rhi
