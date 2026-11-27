#pragma once

// Per-shader state reported by shader hot reload (ROADMAP R2) — what the
// editor's Stats panel lists and what remote commands (A6) return. Kept free
// of Vulkan so UI code can include it cheaply.

#include <cstdint>
#include <string>
#include <string_view>

namespace engine::rhi {

struct ShaderStatus {
    enum class State : std::uint8_t {
        cooked,    // as built offline; not recompiled this session
        compiling, // a recompile is queued or running
        ok,        // last recompile succeeded and its pipelines were swapped
        error,     // last recompile (or pipeline rebuild) failed: old pipelines kept
        // Compiled and installed, but the shader is only used by work that ran
        // once (e.g. a bake pass) and no live pipeline was rebuilt: the change
        // shows after a restart.
        needs_restart,
    };

    std::string   name;    // source path relative to the shader directory ("lib/pbr.slang")
    std::string   source;  // absolute source path
    State         state = State::cooked;
    // Compiler diagnostics: the errors when state == error (plus any pipeline
    // rebuild failure), warnings (possibly empty) when ok; when needs_restart,
    // a note saying so followed by any warnings.
    std::string   message;
    std::uint32_t reloads = 0;   // successful recompiles this session
    std::uint32_t pipelines = 0; // pipelines rebuilt by the last successful reload
    // Finished compile attempts (success or failure) this session: a tool
    // that requested a recompile waits for this to advance (A6).
    std::uint32_t attempts = 0;
};

[[nodiscard]] constexpr std::string_view to_string(ShaderStatus::State state) {
    switch (state) {
    case ShaderStatus::State::cooked:    return "cooked";
    case ShaderStatus::State::compiling: return "compiling";
    case ShaderStatus::State::ok:        return "ok";
    case ShaderStatus::State::error:     return "error";
    case ShaderStatus::State::needs_restart: return "restart";
    }
    return "unknown";
}

} // namespace engine::rhi
