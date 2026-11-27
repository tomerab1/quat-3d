#include "engine/project/build_job.hpp"

#include <algorithm>
#include <charconv>
#include <mutex>
#include <span>
#include <thread>
#include <utility>

#include "engine/core/child_process.hpp"
#include "engine/core/utf8_path.hpp"

namespace engine::project {

namespace {

namespace fs = std::filesystem;

constexpr std::string_view k_prefix = "[pack] ";

[[nodiscard]] std::unexpected<core::Error> fail(std::string what) {
    return std::unexpected(core::Error{std::move(what)});
}

[[nodiscard]] bool parse_u32(std::string_view s, std::uint32_t& out) {
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc() && ptr == s.data() + s.size();
}

// Keeps the last k_build_log_tail_bytes of the log, cut at a line start.
void append_tail(std::string& log, std::string_view chunk) {
    log.append(chunk);
    if (log.size() <= k_build_log_tail_bytes) return;
    std::size_t cut = log.size() - k_build_log_tail_bytes;
    const std::size_t nl = log.find('\n', cut);
    if (nl != std::string::npos) cut = nl + 1;
    log.erase(0, cut);
}

} // namespace

std::string_view host_target() {
#if defined(_WIN32)
    return "windows-x64";
#elif defined(__APPLE__)
    return "macos-arm64";
#else
    return "linux-x64";
#endif
}

std::vector<std::string> build_arguments(const BuildRequest& r) {
    std::vector<std::string> args{core::path_to_utf8(r.project),
                                  "--player",
                                  core::path_to_utf8(r.player),
                                  "--out",
                                  core::path_to_utf8(r.output),
                                  "--config",
                                  std::string(to_string(r.config))};
    if (!r.build_id.empty()) {
        args.emplace_back("--build-id");
        args.push_back(r.build_id);
    }
    if (r.smoke_test) args.emplace_back("--smoke-test");
    return args;
}

std::string_view to_string(BuildState state) {
    switch (state) {
    case BuildState::idle: return "idle";
    case BuildState::running: return "running";
    case BuildState::succeeded: return "succeeded";
    case BuildState::failed: return "failed";
    case BuildState::cancelled: return "cancelled";
    }
    return "idle";
}

float BuildStatus::progress() const {
    if (state == BuildState::succeeded) return 1.0F;
    if (steps == 0 || step == 0) return 0.0F;
    const float done = static_cast<float>(std::min(step, steps) - 1);
    return done / static_cast<float>(steps);
}

// ---- BuildProgress ----------------------------------------------------------

void BuildProgress::feed(std::string_view chunk, BuildStatus& status) {
    append_tail(status.log, chunk);
    pending_.append(chunk);
    std::size_t start = 0;
    for (std::size_t nl = pending_.find('\n'); nl != std::string::npos;
         nl = pending_.find('\n', start)) {
        std::string_view line(pending_.data() + start, nl - start);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        apply_line(line, status);
        start = nl + 1;
    }
    pending_.erase(0, start);
    // A pathological line without newlines must not grow without bound.
    if (pending_.size() > k_build_log_tail_bytes) pending_.clear();
}

void BuildProgress::flush(BuildStatus& status) {
    if (!pending_.empty()) apply_line(pending_, status);
    pending_.clear();
}

void BuildProgress::apply_line(std::string_view line, BuildStatus& status) {
    if (!line.starts_with(k_prefix)) return;
    line.remove_prefix(k_prefix.size());
    if (line.starts_with("step ")) {
        line.remove_prefix(5);
        const std::size_t slash = line.find('/');
        const std::size_t space = line.find(' ');
        std::uint32_t i = 0;
        std::uint32_t n = 0;
        if (slash == std::string_view::npos || space == std::string_view::npos || space < slash ||
            !parse_u32(line.substr(0, slash), i) ||
            !parse_u32(line.substr(slash + 1, space - slash - 1), n) || i == 0 || n == 0) {
            return;
        }
        status.step = i;
        status.steps = n;
        status.step_name = std::string(line.substr(space + 1));
    } else if (line.starts_with("package ")) {
        status.package = std::string(line.substr(8));
    } else if (line.starts_with("folder ")) {
        status.folder = std::string(line.substr(7));
    } else if (line.starts_with("error: ")) {
        if (status.error.empty()) status.error = std::string(line.substr(7));
    }
}

// ---- BuildJob ---------------------------------------------------------------

struct BuildJob::Shared {
    mutable std::mutex                     mutex;
    BuildStatus                            status;
    std::unique_ptr<core::ProcessCanceller> canceller;
    std::thread                            worker;
    std::chrono::steady_clock::time_point  started{};
};

BuildJob::BuildJob() : shared_(std::make_shared<Shared>()) {}

BuildJob::~BuildJob() {
    cancel();
    if (shared_->worker.joinable()) shared_->worker.join();
}

std::expected<std::uint64_t, core::Error> BuildJob::start(BuildRequest request) {
    if (request.quat_pack.empty()) return fail("no quat-pack executable");
    if (request.project.empty()) return fail("no game project");
    if (request.player.empty()) return fail("no quat-player executable");
    if (request.output.empty()) return fail("no output folder");
    std::error_code ec;
    if (!fs::is_regular_file(request.quat_pack, ec)) {
        return fail("quat-pack not found at '" + core::path_to_utf8(request.quat_pack) +
                    "' (build the quat-pack target)");
    }
    if (!fs::is_regular_file(request.player, ec)) {
        return fail("quat-player not found at '" + core::path_to_utf8(request.player) +
                    "' (build the quat-player target)");
    }

    Shared& s = *shared_;
    std::uint64_t job = 0;
    {
        const std::lock_guard lock(s.mutex);
        if (s.status.state == BuildState::running) {
            return fail("a build is already running (job " + std::to_string(s.status.job) + ")");
        }
    }
    // The previous worker has finished (state is not running): reap it.
    if (s.worker.joinable()) s.worker.join();
    {
        const std::lock_guard lock(s.mutex);
        job = s.status.job + 1;
        s.status = BuildStatus{};
        s.status.state = BuildState::running;
        s.status.job = job;
        s.canceller = std::make_unique<core::ProcessCanceller>();
        s.started = std::chrono::steady_clock::now();
    }

    std::shared_ptr<Shared> shared = shared_;
    core::ProcessCanceller* canceller = s.canceller.get();
    s.worker = std::thread([shared, canceller, request = std::move(request)] {
        const std::vector<std::string> args = build_arguments(request);
        BuildProgress parser;
        core::ProcessOptions options;
        options.canceller = canceller;
        options.on_output = [&](std::string_view chunk) {
            const std::lock_guard lock(shared->mutex);
            parser.feed(chunk, shared->status);
        };
        auto result = core::run_process(request.quat_pack, std::span<const std::string>(args),
                                        options);
        const std::lock_guard lock(shared->mutex);
        BuildStatus& st = shared->status;
        parser.flush(st);
        st.elapsed_s = std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                                     shared->started)
                           .count();
        if (canceller->cancelled()) {
            st.state = BuildState::cancelled;
            if (st.error.empty()) st.error = "cancelled";
        } else if (!result) {
            st.state = BuildState::failed;
            st.error = result.error().message;
        } else if (!result->succeeded()) {
            st.state = BuildState::failed;
            if (st.error.empty()) {
                st.error = result->signaled
                               ? "quat-pack crashed (signal " + std::to_string(result->signal) + ")"
                               : "quat-pack exited with code " + std::to_string(result->exit_code);
            }
        } else {
            st.state = BuildState::succeeded;
            st.step = st.steps;
        }
    });
    return job;
}

BuildStatus BuildJob::status(bool with_log) const {
    const std::lock_guard lock(shared_->mutex);
    const BuildStatus& live = shared_->status;
    BuildStatus st;
    st.state = live.state;
    st.job = live.job;
    st.step = live.step;
    st.steps = live.steps;
    st.step_name = live.step_name;
    if (with_log) st.log = live.log;
    st.package = live.package;
    st.folder = live.folder;
    st.error = live.error;
    st.elapsed_s = live.elapsed_s;
    if (st.state == BuildState::running) {
        st.elapsed_s = std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                                     shared_->started)
                           .count();
    }
    return st;
}

bool BuildJob::running() const {
    const std::lock_guard lock(shared_->mutex);
    return shared_->status.state == BuildState::running;
}

void BuildJob::cancel() {
    const std::lock_guard lock(shared_->mutex);
    if (shared_->status.state == BuildState::running && shared_->canceller) {
        shared_->canceller->cancel();
    }
}

} // namespace engine::project
