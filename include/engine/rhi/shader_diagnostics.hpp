#pragma once

// Structured slangc diagnostics (ROADMAP A6): the compiler service returns
// slangc's text output; tools that hand errors to a language model (the
// shader.* remote methods) parse it into located entries so the model can go
// straight to the line. Vulkan-free.
//
// slangc prints one header line per diagnostic, optionally followed by the
// offending source line and a caret line under the exact column:
//
//   /abs/path/materials/lava.slang(12): error 30027: 'uvv' is not a member of 'MaterialInput'.
//           float2 p = input.uvv * 6.0;
//                            ^~~
//   (0): error 45001: unresolved external symbol '...'.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace engine::rhi {

struct ShaderDiagnostic {
    std::string   file;         // as printed (may be empty for link errors)
    std::uint32_t line = 0;     // 1-based; 0 = no source location
    std::uint32_t column = 0;   // 1-based, from the caret line; 0 = unknown
    std::string   severity;     // "error", "warning", "note", "fatal error", ...
    std::uint32_t code = 0;     // slang diagnostic id; 0 when printed without one
    std::string   message;
    std::string   source_line;  // the echoed source line, when printed
};

// Every diagnostic header in `output`, in order. Lines that are not headers
// (echoed source, carets, the service's own notes) are skipped.
[[nodiscard]] std::vector<ShaderDiagnostic> parse_slang_diagnostics(std::string_view output);

} // namespace engine::rhi
