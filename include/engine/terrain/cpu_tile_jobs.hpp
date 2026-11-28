#pragma once

// CpuTileJobs — tile generations a backend hands to CPU workers (issue #78).
//
// A TileGenerator backend that cannot finish a request itself (the GPU
// synthesis pass failing a job) regenerates the tile on a CPU worker and
// fulfils the request's promise from there. Each job keeps the request's
// CancelToken, so the requester cancelling it (a streamer moving on, a
// regenerate) stops the generation at its next cancellation check, exactly
// like a CPU tile started with generate_async(). cancel_all() and the
// destructor cancel every job before joining it, so teardown waits only for
// the workers to notice, never for a full generation and erosion.

#include <cstddef>
#include <future>
#include <vector>

#include "engine/terrain/generator.hpp"
#include "engine/terrain/heightmap.hpp"

namespace engine::terrain {

class CpuTileJobs {
public:
    CpuTileJobs() = default;
    // Cancels and joins every job still running.
    ~CpuTileJobs();
    CpuTileJobs(const CpuTileJobs&) = delete;
    CpuTileJobs& operator=(const CpuTileJobs&) = delete;
    CpuTileJobs(CpuTileJobs&&) noexcept = default;
    CpuTileJobs& operator=(CpuTileJobs&&) noexcept;

    // Generates `params` on a worker and fulfils `promise` with the result:
    // an invalid Heightmap once `cancel` is set. A null `cancel` gets a
    // token of its own, so cancel_all() can still stop the job.
    void start(const TerrainParams& params, std::promise<Heightmap> promise, CancelToken cancel);

    // Sets every running job's token (a requester's token included: the
    // request is given up, as TileGenerator's cancel contract describes).
    // The jobs' promises resolve invalid as each worker stops.
    void cancel_all();

    // Forgets jobs whose worker has finished. Call once per frame.
    void reap();

    // Jobs whose worker has not been reaped yet.
    [[nodiscard]] std::size_t size() const { return jobs_.size(); }

private:
    struct Job {
        std::future<void> done; // from std::async: its destructor joins the worker
        CancelToken       cancel;
    };
    std::vector<Job> jobs_;
};

} // namespace engine::terrain
