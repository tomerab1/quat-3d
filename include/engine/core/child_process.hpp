#pragma once

// Runs a program as a child process and captures its output (ROADMAP R2: the
// shader compiler service runs slangc this way). No shell is involved: the
// arguments are passed to the program verbatim.

#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "engine/core/error.hpp"

namespace engine::core {

struct ProcessResult {
    int         exit_code = -1;  // valid when !signaled
    bool        signaled  = false; // POSIX: terminated by a signal (e.g. a crash)
    int         signal    = 0;
    std::string output;          // stdout and stderr, interleaved as written

    [[nodiscard]] bool succeeded() const { return !signaled && exit_code == 0; }
};

// Runs `program` with `args` (argv[1..]), blocking until it exits. Fails only
// when the process cannot be started; a non-zero exit is a ProcessResult.
// Safe to call from any thread.
[[nodiscard]] std::expected<ProcessResult, Error>
run_process(const std::filesystem::path& program, std::span<const std::string> args);

// Finds an executable named `name` on PATH (".exe" is appended on Windows).
[[nodiscard]] std::optional<std::filesystem::path> find_on_path(std::string_view name);

} // namespace engine::core
