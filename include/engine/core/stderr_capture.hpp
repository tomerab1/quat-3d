#pragma once

// StderrCapture — tees everything the process writes to stderr into a LogRing
// (ROADMAP A5), so remote debugging (`debug.log`) sees today's
// fprintf(stderr, ...) output, validation layers and third-party libraries,
// not only lines written through core::log().
//
// start() points file descriptor 2 at a pipe. A dedicated drain thread reads
// the pipe as soon as data arrives, forwards the bytes unchanged to the
// original stderr (the terminal or log file keeps working) and pushes each
// complete line, parsed by parse_stderr_line(), into the ring. Draining on
// its own thread rather than once per frame means a writer can never fill
// the pipe and block: not during a long main-thread stall (scene load, a
// navmesh bake), and not when the main thread itself writes a burst. The
// thread touches only the ring and the two descriptors — never engine state.
//
// stop() (or the destructor) restores fd 2, drains what is left and joins the
// thread. Only one capture can be active per process. POSIX only for now;
// start() returns an error elsewhere and logging keeps working uncaptured.

#include <cstddef>
#include <expected>
#include <memory>
#include <string_view>

#include "engine/core/error.hpp"

namespace engine::core {

class LogRing;

class StderrCapture {
public:
    [[nodiscard]] static std::expected<StderrCapture, Error> start(LogRing& ring);

    StderrCapture(); // inactive
    StderrCapture(StderrCapture&&) noexcept;
    StderrCapture& operator=(StderrCapture&&) noexcept;
    StderrCapture(const StderrCapture&) = delete;
    StderrCapture& operator=(const StderrCapture&) = delete;
    ~StderrCapture();

    [[nodiscard]] bool active() const { return impl_ != nullptr; }

    // Blocks until every byte written to stderr before the call has been
    // forwarded and its complete lines are in the ring (tests, shutdown).
    void flush();

    // Restores the original stderr. Idempotent.
    void stop();

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

// Writes `text` to the process's real stderr: the original descriptor while
// a capture is active (so the text is not captured a second time), fd 2
// otherwise. Thread-safe; never blocks on the capture pipe.
void write_real_stderr(std::string_view text);

} // namespace engine::core
