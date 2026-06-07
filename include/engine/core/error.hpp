#pragma once

#include <string>
#include <utility>

namespace engine::core {

// Lightweight error payload carried by std::expected throughout the engine.
// We do not use exceptions (see CLAUDE.md); fallible functions return
// std::expected<T, Error> instead.
struct Error {
    std::string message;
};

// Convenience for building an error in a `return std::unexpected(...)` site.
[[nodiscard]] inline Error make_error(std::string message) {
    return Error{std::move(message)};
}

} // namespace engine::core
