#include "engine/rhi/shader_hot_reload.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

#include "engine/core/utf8_path.hpp"
#include "engine/core/child_process.hpp"
#include "engine/rhi/pipeline_cache.hpp"
#include "engine/rhi/shader_compiler.hpp"

namespace engine::rhi {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

// One source to compile, plus the sources it imports that are compiled earlier
// in the same batch (if one of those fails, this one is skipped).
struct CompileJob {
    fs::path              source;
    std::vector<fs::path> batch_imports;
};

[[nodiscard]] std::vector<ShaderCompileResult>
run_batch(const ShaderCompiler& compiler, const std::vector<CompileJob>& jobs,
          core::ProcessCanceller* canceller) {
    std::vector<ShaderCompileResult> results;
    std::set<fs::path> failed;
    for (const CompileJob& job : jobs) {
        const auto bad = std::find_if(job.batch_imports.begin(), job.batch_imports.end(),
                                      [&](const fs::path& p) { return failed.contains(p); });
        if (bad != job.batch_imports.end()) {
            ShaderCompileResult skipped;
            skipped.source = job.source;
            skipped.kind = compiler.kind_of(job.source);
            skipped.diagnostics = "not recompiled: imported module " +
                                  core::path_to_utf8(bad->filename()) + " failed to compile";
            failed.insert(job.source);
            results.push_back(std::move(skipped));
            continue;
        }
        ShaderCompileResult r = compiler.compile(job.source, canceller);
        if (!r.ok) failed.insert(job.source);
        results.push_back(std::move(r));
    }
    return results;
}

[[nodiscard]] std::string trimmed(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    std::size_t start = 0;
    while (start < s.size() && (s[start] == '\n' || s[start] == '\r')) ++start;
    return s.substr(start);
}

} // namespace

struct ShaderHotReload::Impl {
    PipelineCache&        cache;
    Config                config;
    ShaderCompiler        compiler;
    ShaderDependencyGraph graph;
    core::FileWatcher     watcher;

    std::vector<ShaderStatus>             statuses; // sorted by name
    std::map<fs::path, Clock::time_point> pending;  // changed, waiting to settle

    // Worker thread state.
    std::mutex                              mutex;
    std::condition_variable                 wake;
    std::deque<std::vector<CompileJob>>     queue;       // guarded by mutex
    std::vector<ShaderCompileResult>        results;     // guarded by mutex
    bool                                    stop = false; // guarded by mutex
    std::atomic<bool>                       has_results{false};
    std::atomic<int>                        batches_in_flight{0};
    // Kills the worker's running slangc at shutdown, so destruction never
    // waits for a slow or hung compile.
    core::ProcessCanceller                  canceller;
    std::thread                             worker;

    Impl(PipelineCache& c, Config cfg, ShaderCompiler comp, core::FileWatcher w)
        : cache(c), config(std::move(cfg)), compiler(std::move(comp)),
          graph(compiler.config().source_dir, compiler.config().module_dir,
                compiler.config().material_dir),
          watcher(std::move(w)) {}

    ~Impl() {
        {
            std::lock_guard lock(mutex);
            stop = true;
        }
        canceller.cancel();
        wake.notify_all();
        if (worker.joinable()) worker.join();
    }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    void worker_main() {
        for (;;) {
            std::vector<CompileJob> jobs;
            {
                std::unique_lock lock(mutex);
                wake.wait(lock, [&] { return stop || !queue.empty(); });
                if (stop) return;
                jobs = std::move(queue.front());
                queue.pop_front();
            }
            std::vector<ShaderCompileResult> done = run_batch(compiler, jobs, &canceller);
            {
                std::lock_guard lock(mutex);
                for (ShaderCompileResult& r : done) results.push_back(std::move(r));
            }
            has_results.store(true, std::memory_order_release);
            batches_in_flight.fetch_sub(1, std::memory_order_acq_rel);
        }
    }

    [[nodiscard]] std::string display_name(const fs::path& source) const {
        const fs::path rel = source.lexically_relative(compiler.config().source_dir);
        return rel.empty() ? core::path_to_generic_utf8(source.filename()) : core::path_to_generic_utf8(rel);
    }

    ShaderStatus& status_for(const fs::path& source) {
        const std::string name = display_name(source);
        auto it = std::lower_bound(statuses.begin(), statuses.end(), name,
                                   [](const ShaderStatus& s, const std::string& n) {
                                       return s.name < n;
                                   });
        if (it == statuses.end() || it->name != name) {
            ShaderStatus s;
            s.name = name;
            s.source = core::path_to_utf8(source);
            it = statuses.insert(it, std::move(s));
        }
        return *it;
    }

    void drop_status(const fs::path& source) {
        const std::string name = display_name(source);
        std::erase_if(statuses, [&](const ShaderStatus& s) { return s.name == name; });
    }

    void handle_changes(Clock::time_point now) {
        for (const core::FileChange& change : watcher.poll()) {
            if (change.kind == core::FileChange::Kind::removed) {
                // Recompile the importers so the missing module is reported.
                const fs::path removed[] = {change.path};
                for (const fs::path& p : graph.rebuild_order(removed)) {
                    if (p != change.path) pending[p] = now;
                }
                graph.remove_file(change.path);
                pending.erase(change.path);
                drop_status(change.path);
                continue;
            }
            pending[change.path] = now;
        }
    }

    void dispatch(Clock::time_point now) {
        std::vector<fs::path> settled;
        for (auto it = pending.begin(); it != pending.end();) {
            if (now - it->second >= config.debounce) {
                settled.push_back(it->first);
                it = pending.erase(it);
            } else {
                ++it;
            }
        }
        if (settled.empty()) return;

        for (const fs::path& p : settled) graph.update_file(p); // imports may have changed
        const std::vector<fs::path> order = graph.rebuild_order(settled);
        if (order.empty()) return;

        const std::set<fs::path> in_batch(order.begin(), order.end());
        std::vector<CompileJob> jobs;
        for (const fs::path& source : order) {
            CompileJob job{source, {}};
            for (const fs::path& dep : graph.imports_of(source)) {
                if (in_batch.contains(dep)) job.batch_imports.push_back(dep);
            }
            jobs.push_back(std::move(job));
            status_for(source).state = ShaderStatus::State::compiling;
        }

        if (!config.async) {
            for (const ShaderCompileResult& r : run_batch(compiler, jobs, &canceller)) apply(r);
            return;
        }
        batches_in_flight.fetch_add(1, std::memory_order_acq_rel);
        {
            std::lock_guard lock(mutex);
            queue.push_back(std::move(jobs));
        }
        wake.notify_one();
    }

    void collect_results() {
        if (!has_results.exchange(false, std::memory_order_acq_rel)) return;
        std::vector<ShaderCompileResult> done;
        {
            std::lock_guard lock(mutex);
            done.swap(results);
        }
        for (const ShaderCompileResult& r : done) {
            // Removed while it compiled: handle_changes() dropped its status;
            // don't bring it back.
            if (graph.contains(r.source)) apply(r);
        }
    }

    void apply(const ShaderCompileResult& r) {
        ShaderStatus& status = status_for(r.source);
        ++status.attempts;
        const std::string diagnostics = trimmed(r.diagnostics);
        if (!r.ok) {
            status.state = ShaderStatus::State::error;
            status.message = diagnostics.empty() ? "compilation failed" : diagnostics;
            std::fprintf(stderr, "[shader] %s: compile failed, keeping the previous version\n%s\n",
                         status.name.c_str(), status.message.c_str());
            return;
        }
        if (r.kind == ShaderSourceKind::module) {
            status.state = ShaderStatus::State::ok;
            status.message = diagnostics;
            status.pipelines = 0;
            ++status.reloads;
            std::fprintf(stderr, "[shader] %s: module recompiled\n", status.name.c_str());
            return;
        }

        // Pass or material shader: reload every installed program (.spv).
        std::size_t rebuilt = 0;
        bool in_use = false;
        std::vector<std::string> errors;
        for (const fs::path& out : r.outputs) {
            if (out.extension() != ".spv") continue;
            auto report = cache.reload(core::path_to_utf8(out));
            if (!report) {
                errors.push_back("compiled, but loading the new SPIR-V failed: " +
                                 report.error().message);
                continue;
            }
            in_use = in_use || report->shader_in_use;
            rebuilt += report->rebuilt;
            for (const std::string& e : report->errors) errors.push_back(e);
        }
        status.pipelines = static_cast<std::uint32_t>(rebuilt);
        if (errors.empty() && r.kind == ShaderSourceKind::pass && in_use && rebuilt == 0) {
            // Loaded by work that ran once (a bake pass) and holds no live
            // pipeline: nothing on screen changes until that work runs again.
            // (A material program without a pipeline is picked up by the
            // pass's next pipeline build instead.)
            status.state = ShaderStatus::State::needs_restart;
            status.message = "compiled; no live pipeline uses it - restart to apply";
            ++status.reloads;
            std::fprintf(stderr, "[shader] %s: %s\n", status.name.c_str(), status.message.c_str());
            return;
        }
        if (errors.empty()) {
            // Not loaded yet (the next load reads the new file) or swapped in.
            status.state = ShaderStatus::State::ok;
            status.message = diagnostics;
            ++status.reloads;
            std::fprintf(stderr, "[shader] %s: reloaded (%u pipeline%s)\n", status.name.c_str(),
                         status.pipelines, status.pipelines == 1 ? "" : "s");
            return;
        }
        status.state = ShaderStatus::State::error;
        status.message = "compiled, but kept the previous pipeline:";
        for (const std::string& e : errors) status.message += "\n" + e;
        if (!diagnostics.empty()) status.message += "\n" + diagnostics;
        std::fprintf(stderr, "[shader] %s: %s\n", status.name.c_str(), status.message.c_str());
    }
};

bool ShaderHotReload::enabled_by_environment() {
    if (const char* v = std::getenv("QUAT_SHADER_HOT_RELOAD"); v != nullptr && *v != '\0') {
        return std::strcmp(v, "0") != 0 && std::strcmp(v, "false") != 0 &&
               std::strcmp(v, "off") != 0;
    }
    // Default: on in editor builds run from the source tree; off in packaged
    // builds (ENGINE_PACKAGE_BUILD), which ship no slangc.
#if defined(ENGINE_EDITOR) && !defined(ENGINE_PACKAGE_BUILD)
    return true;
#else
    return false;
#endif
}

std::expected<std::unique_ptr<ShaderHotReload>, core::Error>
ShaderHotReload::create(PipelineCache& cache, Config config) {
    auto compiler = ShaderCompiler::create({config.slangc, config.source_dir, config.module_dir,
                                            config.output_dir, config.material_dir,
                                            config.compile_timeout});
    if (!compiler) return std::unexpected(compiler.error());

    core::FileWatcher::Options watch;
    watch.directories.push_back(compiler->config().source_dir);
    std::error_code ec;
    if (fs::is_directory(compiler->config().module_dir, ec)) {
        watch.directories.push_back(compiler->config().module_dir);
    }
    // Material shaders (A6): created on demand so materials authored at
    // runtime are watched from the start.
    fs::create_directories(compiler->config().material_dir, ec);
    if (fs::is_directory(compiler->config().material_dir, ec)) {
        watch.directories.push_back(compiler->config().material_dir);
    }
    watch.extensions = {".slang"};
    watch.backend = config.watch_backend;
    watch.poll_interval = config.poll_interval;
    auto watcher = core::FileWatcher::create(std::move(watch));
    if (!watcher) return std::unexpected(watcher.error());

    cache.set_frames_in_flight(config.frames_in_flight);
    auto impl = std::make_unique<Impl>(cache, std::move(config), std::move(*compiler),
                                       std::move(*watcher));
    impl->graph.scan();
    for (const fs::path& source : impl->graph.sources()) impl->status_for(source);
    if (impl->config.async) {
        impl->worker = std::thread([raw = impl.get()] { raw->worker_main(); });
    }
    return std::make_unique<ShaderHotReload>(PrivateTag{}, std::move(impl));
}

std::unique_ptr<ShaderHotReload> ShaderHotReload::create_from_environment(PipelineCache& cache,
                                                                          Config config) {
    if (!enabled_by_environment()) return nullptr;
    auto reload = create(cache, std::move(config));
    if (!reload) {
        std::fprintf(stderr, "[shader] hot reload disabled: %s\n", reload.error().message.c_str());
        return nullptr;
    }
    std::fprintf(stderr, "[shader] hot reload on (%s watcher, %s)\n",
                 (*reload)->watch_backend() == core::FileWatchBackend::native ? "native"
                                                                              : "polling",
                 core::path_to_utf8((*reload)->impl_->compiler.config().slangc).c_str());
    return std::move(*reload);
}

std::unique_ptr<ShaderHotReload>
ShaderHotReload::create_from_environment(PipelineCache& cache, const fs::path& source_dir,
                                         const fs::path& output_dir) {
    Config config;
    config.source_dir = source_dir;
    config.output_dir = output_dir;
    return create_from_environment(cache, std::move(config));
}

ShaderHotReload::ShaderHotReload(PrivateTag, std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}
ShaderHotReload::~ShaderHotReload() = default;

void ShaderHotReload::update(std::uint64_t frame_number) {
    impl_->cache.begin_frame(frame_number);
    const Clock::time_point now = Clock::now();
    impl_->handle_changes(now);
    impl_->dispatch(now);
    impl_->collect_results();
}

bool ShaderHotReload::request_recompile(const fs::path& source) {
    const fs::path key = source.lexically_normal();
    if (!impl_->graph.contains(key)) {
        const ShaderCompilerConfig& c = impl_->compiler.config();
        const fs::path dir = key.parent_path();
        std::error_code ec;
        const bool watched = dir == c.source_dir || dir == c.module_dir || dir == c.material_dir;
        if (!watched || key.extension() != ".slang" || !fs::is_regular_file(key, ec)) {
            return false;
        }
        impl_->graph.update_file(key);
        impl_->status_for(key);
    }
    // Back-date so the next update() dispatches it without waiting to settle.
    impl_->pending[key] = Clock::now() - impl_->config.debounce;
    return true;
}

const std::vector<ShaderStatus>& ShaderHotReload::statuses() const { return impl_->statuses; }

const ShaderStatus* ShaderHotReload::status_of(const fs::path& source) const {
    const std::string name = impl_->display_name(source.lexically_normal());
    const auto it = std::lower_bound(impl_->statuses.begin(), impl_->statuses.end(), name,
                                     [](const ShaderStatus& s, const std::string& n) {
                                         return s.name < n;
                                     });
    return it != impl_->statuses.end() && it->name == name ? &*it : nullptr;
}

const ShaderCompilerConfig& ShaderHotReload::compiler_config() const {
    return impl_->compiler.config();
}

std::size_t ShaderHotReload::error_count() const {
    return static_cast<std::size_t>(
        std::count_if(impl_->statuses.begin(), impl_->statuses.end(), [](const ShaderStatus& s) {
            return s.state == ShaderStatus::State::error;
        }));
}

bool ShaderHotReload::idle() const {
    return impl_->pending.empty() &&
           impl_->batches_in_flight.load(std::memory_order_acquire) == 0 &&
           !impl_->has_results.load(std::memory_order_acquire);
}

core::FileWatchBackend ShaderHotReload::watch_backend() const { return impl_->watcher.backend(); }

} // namespace engine::rhi
