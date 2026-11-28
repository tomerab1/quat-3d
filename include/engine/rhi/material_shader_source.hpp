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
// It must not take the GPU down either (issue #110): a material runs for every
// GBuffer pixel, and one that touches memory it does not own or never ends
// loses the device. So it may also not contain
//   * inline SPIR-V (`spirv_asm`) or `vk::` attributes / intrinsics;
//   * pointers: pointer types (`float*`, `T*` for a struct it declares,
//     `vector<..>*`, casts `(T*)` / `bit_cast<T*>`), `Ptr<T>` and friends, a
//     dereference `*p`, an address `&x`, or `p->field`;
//   * buffer device addresses (`ConstBufferPointer`, `RawBufferLoad`,
//     `buffer_reference`);
//   * loops that never end by themselves: `for (...; ; ...)`, `while (true)`.
// Loops need compile-time bounds; Slang has no attribute that caps a loop's
// iterations at run time, so that rule is documented for authors (MCP shader
// tool descriptions) rather than enforced.
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
