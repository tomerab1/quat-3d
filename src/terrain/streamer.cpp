#include "engine/terrain/streamer.hpp"

#include <chrono>
#include <cmath>
#include <limits>
#include <string>

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

std::expected<void, core::Error> run_terrain_streaming_self_test() {
    const auto fail = [](const char* what) {
        return std::unexpected(core::Error{std::string(what)});
    };

    TerrainParams base;
    base.resolution = 65;
    base.tile_size_m = 64.0F;
    base.height_m = 10.0F;
    base.octaves = 4;
    base.erosion_droplets = 2'000; // exercises the edge fade
    TerrainStreamer streamer(base, 1);

    auto events = streamer.update(glm::vec2(0.0F), /*block=*/true);
    if (streamer.maps().size() != 9 || events.ready.size() != 9) {
        return fail("terrain streaming self-test: window did not fill (expected 3x3)");
    }

    // Seam: tile (0,0)'s +x edge must equal tile (1,0)'s -x edge bit-exactly.
    const Heightmap& a = streamer.maps().at(glm::ivec2(0, 0));
    const Heightmap& b = streamer.maps().at(glm::ivec2(1, 0));
    const std::uint32_t res = a.resolution;
    for (std::uint32_t z = 0; z < res; ++z) {
        const float ha = a.heights[static_cast<std::size_t>(z) * res + (res - 1)];
        const float hb = b.heights[static_cast<std::size_t>(z) * res + 0];
        if (ha != hb) {
            return fail("terrain streaming self-test: adjacent tiles diverge at the seam");
        }
    }

    // Follow the camera one tile east: a new column loads, the far one drops.
    events = streamer.update(glm::vec2(base.tile_size_m, 0.0F), /*block=*/true);
    if (streamer.maps().size() != 9) {
        return fail("terrain streaming self-test: window size changed after a move");
    }
    if (events.removed.size() != 3 || events.ready.size() != 3) {
        return fail("terrain streaming self-test: expected one column swapped");
    }
    if (!streamer.maps().contains(glm::ivec2(2, 0)) ||
        streamer.maps().contains(glm::ivec2(-1, 0))) {
        return fail("terrain streaming self-test: window not centred on the new tile");
    }
    return {};
}

} // namespace engine::terrain
