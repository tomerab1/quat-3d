#include "engine/terrain/streamer.hpp"

#include <chrono>
#include <cmath>
#include <limits>

namespace engine::terrain {

TerrainStreamer::Events TerrainStreamer::update(glm::vec2 local_xz, bool block) {
    Events events;
    const float t = base_.tile_size_m;
    // Tile (0,0) is centred on the anchor: its span is [-t/2, t/2).
    const glm::ivec2 centre(static_cast<int>(std::floor(local_xz.x / t + 0.5F)),
                            static_cast<int>(std::floor(local_xz.y / t + 0.5F)));

    // Drop tiles outside the window.
    for (auto it = maps_.begin(); it != maps_.end();) {
        const glm::ivec2 d = it->first - centre;
        if (glm::abs(d.x) > radius_ || glm::abs(d.y) > radius_) {
            events.removed.push_back(it->first);
            it = maps_.erase(it);
        } else {
            ++it;
        }
    }

    do {
        // Collect a finished job.
        if (job_active_ &&
            (block || job_.wait_for(std::chrono::seconds(0)) == std::future_status::ready)) {
            Heightmap map = job_.get();
            job_active_ = false;
            const glm::ivec2 d = job_coord_ - centre;
            if (glm::abs(d.x) <= radius_ && glm::abs(d.y) <= radius_) {
                maps_.emplace(job_coord_, std::move(map));
                events.ready.push_back(job_coord_);
            } // else: the camera outran the job — discard.
        }

        // Kick the nearest missing tile (one job in flight).
        if (!job_active_) {
            glm::ivec2 best{0, 0};
            int best_d2 = std::numeric_limits<int>::max();
            for (int dz = -radius_; dz <= radius_; ++dz) {
                for (int dx = -radius_; dx <= radius_; ++dx) {
                    const glm::ivec2 c = centre + glm::ivec2(dx, dz);
                    if (maps_.contains(c)) continue;
                    const int d2 = dx * dx + dz * dz;
                    if (d2 < best_d2) {
                        best_d2 = d2;
                        best = c;
                    }
                }
            }
            if (best_d2 == std::numeric_limits<int>::max()) break; // window full
            TerrainParams params = base_;
            params.world_tile = best;
            job_coord_ = best;
            job_ = generate_async(params);
            job_active_ = true;
        }
    } while (block); // blocking mode drains the whole window synchronously

    return events;
}

} // namespace engine::terrain
