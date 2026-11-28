#include "engine/terrain/cpu_tile_jobs.hpp"

#include <atomic>
#include <chrono>
#include <utility>

namespace engine::terrain {

CpuTileJobs::~CpuTileJobs() {
    // Cancel first: the futures' destructors join their workers, which then
    // only have to reach their next cancellation check.
    cancel_all();
}

CpuTileJobs& CpuTileJobs::operator=(CpuTileJobs&& other) noexcept {
    if (this != &other) {
        cancel_all();
        jobs_ = std::move(other.jobs_);
    }
    return *this;
}

void CpuTileJobs::start(const TerrainParams& params, std::promise<Heightmap> promise,
                        CancelToken cancel) {
    if (!cancel) cancel = make_cancel_token();
    Job job;
    job.cancel = cancel;
    job.done = std::async(std::launch::async, [promise = std::move(promise), params,
                                               cancel = std::move(cancel)]() mutable {
        promise.set_value(generate(params, cancel.get()));
    });
    jobs_.push_back(std::move(job));
}

void CpuTileJobs::cancel_all() {
    for (const Job& job : jobs_) job.cancel->store(true, std::memory_order_relaxed);
}

void CpuTileJobs::reap() {
    std::erase_if(jobs_, [](const Job& job) {
        return job.done.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
    });
}

} // namespace engine::terrain
