// Terrain edit stamps (G19.1): validation, bounds, coverage, the ordered
// list and its diff. Per-tile application is in edits_apply.cpp.

#include "engine/terrain/edits.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <set>
#include <unordered_map>
#include <utility>

namespace engine::terrain {

namespace {

constexpr std::array<const char*, 3> k_shapes{"circle", "capsule", "polygon"};
constexpr std::array<const char*, 5> k_ops{"flatten", "raise", "lower", "smooth", "paint"};
constexpr std::array<const char*, 4> k_channels{"dirt", "gravel", "soil", "grass"};
// Steepest flatten plane: normal.y below this (about 84 degrees) is refused.
constexpr float k_min_plane_normal_y = 0.1F;

template <class E, std::size_t N>
std::optional<E> parse(std::string_view name, const std::array<const char*, N>& names) {
    for (std::size_t i = 0; i < N; ++i) {
        if (name == names[i]) return static_cast<E>(i);
    }
    return std::nullopt;
}

[[nodiscard]] bool finite(float v) { return std::isfinite(v); }
[[nodiscard]] bool finite(glm::vec2 v) { return finite(v.x) && finite(v.y); }

[[nodiscard]] std::unexpected<std::string> bad(std::string what) {
    return std::unexpected(std::move(what));
}

[[nodiscard]] float lerp(float x, float y, float a) { return x * (1.0F - a) + y * a; }

} // namespace

const char* to_string(StampShape shape) { return k_shapes[static_cast<std::size_t>(shape)]; }
const char* to_string(StampOp op) { return k_ops[static_cast<std::size_t>(op)]; }
const char* to_string(PaintChannel channel) {
    return k_channels[static_cast<std::size_t>(channel)];
}
std::optional<StampShape> stamp_shape_from_string(std::string_view name) {
    return parse<StampShape>(name, k_shapes);
}
std::optional<StampOp> stamp_op_from_string(std::string_view name) {
    return parse<StampOp>(name, k_ops);
}
std::optional<PaintChannel> paint_channel_from_string(std::string_view name) {
    return parse<PaintChannel>(name, k_channels);
}

std::expected<void, std::string> validate_stamp(const TerrainStamp& s) {
    if (static_cast<std::size_t>(s.shape) >= k_shapes.size()) return bad("shape: unknown");
    if (static_cast<std::size_t>(s.op) >= k_ops.size()) return bad("op: unknown");
    if (static_cast<std::size_t>(s.channel) >= k_channels.size()) return bad("channel: unknown");
    const std::size_t need = s.shape == StampShape::polygon ? 3 : 1;
    if (s.points.size() < need || s.points.size() > max_stamp_points) {
        return bad(std::string("points: a ") + to_string(s.shape) + " needs " +
                   std::to_string(need) + " to " + std::to_string(max_stamp_points) + " points" +
                   (s.shape == StampShape::circle ? " (the centre first)" : ""));
    }
    if (s.shape == StampShape::circle && s.points.size() != 1) {
        return bad("points: a circle takes exactly one point (its centre)");
    }
    for (std::size_t i = 0; i < s.points.size(); ++i) {
        const glm::vec2 p = s.points[i];
        if (!finite(p) || std::abs(p.x) > max_stamp_coordinate_m ||
            std::abs(p.y) > max_stamp_coordinate_m) {
            return bad("points[" + std::to_string(i) + "]: expected finite coordinates within " +
                       std::to_string(static_cast<int>(max_stamp_coordinate_m)) + " m");
        }
    }
    if (!finite(s.radius) || s.radius < 0.0F) return bad("radius: expected >= 0");
    if (!finite(s.falloff) || s.falloff < 0.0F) return bad("falloff: expected >= 0");
    if (s.radius + s.falloff > max_stamp_extent_m) {
        return bad("radius + falloff: at most " +
                   std::to_string(static_cast<int>(max_stamp_extent_m)) + " m");
    }
    if (s.shape == StampShape::circle && !(s.radius + s.falloff > 0.0F)) {
        return bad("radius: a circle needs radius + falloff > 0");
    }
    if (s.shape == StampShape::capsule && !(s.radius + s.falloff > 0.0F)) {
        return bad("radius: a capsule needs radius + falloff > 0");
    }
    if (!finite(s.strength) || s.strength < 0.0F || s.strength > 1.0F) {
        return bad("strength: expected 0..1");
    }
    if (!finite(s.amount) || s.amount < 0.0F || s.amount > max_stamp_amount_m) {
        return bad("amount: expected 0.." + std::to_string(static_cast<int>(max_stamp_amount_m)) +
                   " m (raise / lower choose the direction)");
    }
    if (!finite(s.height) || std::abs(s.height) > max_stamp_coordinate_m) {
        return bad("height: expected a finite height");
    }
    if (!finite(s.pivot) || std::abs(s.pivot.x) > max_stamp_coordinate_m ||
        std::abs(s.pivot.y) > max_stamp_coordinate_m) {
        return bad("pivot: expected finite coordinates");
    }
    const float len = glm::length(s.normal);
    if (!std::isfinite(len) || !(len > 0.0F) || s.normal.y / len < k_min_plane_normal_y) {
        return bad("normal: expected an upward plane normal (y > 0.1 of its length)");
    }
    if (s.iterations < 1 || s.iterations > max_smooth_iterations) {
        return bad("iterations: expected 1.." + std::to_string(max_smooth_iterations));
    }
    if (!finite(s.value) || s.value < 0.0F || s.value > 1.0F) return bad("value: expected 0..1");
    return {};
}

EditRect stamp_bounds(const TerrainStamp& s) {
    EditRect r{glm::vec2(std::numeric_limits<float>::max()),
               glm::vec2(std::numeric_limits<float>::lowest())};
    for (const glm::vec2& p : s.points) {
        r.min = glm::min(r.min, p);
        r.max = glm::max(r.max, p);
    }
    const float grow = s.radius + s.falloff;
    r.min -= glm::vec2(grow);
    r.max += glm::vec2(grow);
    return r;
}

float stamp_coverage(const TerrainStamp& stamp, glm::vec2 p) {
    const std::span<const TerrainStamp> one(&stamp, 1);
    const PackedStamps packed = pack_stamps(one);
    return packed_coverage(packed, packed.stamps.front(), p);
}

// ---------------------------------------------------------------------------
// Packing + coverage (mirrored op for op by terrain_edit.slang)
// ---------------------------------------------------------------------------

PackedStamps pack_stamps(std::span<const TerrainStamp> stamps) {
    PackedStamps out;
    out.stamps.reserve(stamps.size());
    for (const TerrainStamp& s : stamps) {
        PackedStamp p;
        p.op = static_cast<std::uint32_t>(s.op);
        p.shape = static_cast<std::uint32_t>(s.shape);
        p.first = static_cast<std::uint32_t>(out.points.size());
        p.count = static_cast<std::uint32_t>(s.points.size());
        p.radius = s.radius;
        p.falloff = s.falloff;
        p.inv_falloff = s.falloff > 0.0F ? 1.0F / s.falloff : 0.0F;
        p.strength = s.strength;
        p.amount = s.op == StampOp::lower ? -s.amount : s.amount;
        p.height = s.height;
        p.pivot = s.pivot;
        const glm::vec3 n = glm::normalize(s.normal);
        p.slope = glm::vec2(-n.x / n.y, -n.z / n.y);
        p.channel = static_cast<std::uint32_t>(s.channel);
        p.value = s.value;
        const EditRect b = stamp_bounds(s);
        p.bounds = glm::vec4(b.min, b.max);
        const std::size_t n_pts = s.points.size();
        for (std::size_t i = 0; i < n_pts; ++i) {
            out.points.push_back(s.points[i]);
            // Capsule: segment i -> i + 1. Polygon: segment i -> i - 1
            // (wrapping), the form of Quilez's sdPolygon winding test.
            glm::vec2 e(0.0F);
            if (s.shape == StampShape::polygon) {
                e = s.points[(i + n_pts - 1) % n_pts] - s.points[i];
            } else if (i + 1 < n_pts) {
                e = s.points[i + 1] - s.points[i];
            }
            const float ee = glm::dot(e, e);
            out.edges.emplace_back(e, ee > 0.0F ? 1.0F / ee : 0.0F, 0.0F);
        }
        out.stamps.push_back(p);
    }
    return out;
}

float packed_coverage(const PackedStamps& packed, const PackedStamp& s, glm::vec2 p) {
    float d = 0.0F; // signed distance to the shape before the radius outset
    const glm::vec2* pts = packed.points.data() + s.first;
    const glm::vec4* edges = packed.edges.data() + s.first;
    if (s.shape == static_cast<std::uint32_t>(StampShape::circle)) {
        d = glm::length(p - pts[0]);
    } else if (s.shape == static_cast<std::uint32_t>(StampShape::capsule)) {
        float best = glm::dot(p - pts[0], p - pts[0]);
        for (std::uint32_t i = 0; i + 1 < s.count; ++i) {
            const glm::vec2 e(edges[i].x, edges[i].y);
            const glm::vec2 w = p - pts[i];
            const float h = glm::clamp(glm::dot(w, e) * edges[i].z, 0.0F, 1.0F);
            const glm::vec2 b = w - e * h;
            best = std::min(best, glm::dot(b, b));
        }
        d = std::sqrt(best);
    } else {
        // sdPolygon (Quilez): squared distance to the nearest edge, sign from
        // a crossing-number test written without division.
        float best = glm::dot(p - pts[0], p - pts[0]);
        float sign = 1.0F;
        for (std::uint32_t i = 0; i < s.count; ++i) {
            const glm::vec2 e(edges[i].x, edges[i].y); // v[i - 1] - v[i]
            const glm::vec2 w = p - pts[i];
            const float h = glm::clamp(glm::dot(w, e) * edges[i].z, 0.0F, 1.0F);
            const glm::vec2 b = w - e * h;
            best = std::min(best, glm::dot(b, b));
            const glm::vec2 prev = pts[i] + e;
            const bool c0 = p.y >= pts[i].y;
            const bool c1 = p.y < prev.y;
            const bool c2 = e.x * w.y > e.y * w.x;
            if ((c0 && c1 && c2) || (!c0 && !c1 && !c2)) sign = -sign;
        }
        d = sign * std::sqrt(best);
    }
    d -= s.radius;
    if (d <= 0.0F) return 1.0F;
    if (s.inv_falloff == 0.0F) return 0.0F;
    const float t = std::min(d * s.inv_falloff, 1.0F);
    return 1.0F - t * t * (3.0F - 2.0F * t);
}

void apply_pointwise(const PackedStamps& packed, const PackedStamp& s, glm::vec2 p, float& height,
                     std::uint32_t& paint) {
    if (p.x < s.bounds.x || p.y < s.bounds.y || p.x > s.bounds.z || p.y > s.bounds.w) return;
    const float coverage = packed_coverage(packed, s, p);
    if (coverage <= 0.0F) return;
    const float a = coverage * s.strength;
    switch (static_cast<StampOp>(s.op)) {
    case StampOp::flatten: {
        const float target =
            s.height + s.slope.x * (p.x - s.pivot.x) + s.slope.y * (p.y - s.pivot.y);
        height = lerp(height, target, a);
        break;
    }
    case StampOp::raise:
    case StampOp::lower:
        height = height + s.amount * a;
        break;
    case StampOp::paint: {
        const std::uint32_t shift = 8U * s.channel;
        const float current = static_cast<float>((paint >> shift) & 0xFFU) * (1.0F / 255.0F);
        const float v = lerp(current, s.value, a);
        const auto byte =
            static_cast<std::uint32_t>(std::clamp(v * 255.0F + 0.5F, 0.0F, 255.0F));
        paint = (paint & ~(0xFFU << shift)) | (byte << shift);
        break;
    }
    case StampOp::smooth:
        break; // a neighbourhood operation: see apply_edits
    }
}

// ---------------------------------------------------------------------------
// TerrainEditList
// ---------------------------------------------------------------------------

std::expected<std::uint64_t, std::string>
TerrainEditList::add(TerrainStamp stamp, std::optional<std::uint64_t> before) {
    if (stamps_.size() >= max_stamps) {
        return bad("the edit list is full (" + std::to_string(max_stamps) +
                   " stamps); remove or merge stamps first");
    }
    if (auto ok = validate_stamp(stamp); !ok) return std::unexpected(ok.error());
    auto at = stamps_.end();
    if (before) {
        at = std::ranges::find(stamps_, *before, &TerrainStamp::id);
        if (at == stamps_.end()) {
            return bad("before: no stamp with id " + std::to_string(*before));
        }
    }
    stamp.id = next_id_++;
    const std::uint64_t id = stamp.id;
    stamps_.insert(at, std::move(stamp));
    ++revision_;
    return id;
}

std::expected<void, std::string> TerrainEditList::update(std::uint64_t id, TerrainStamp stamp) {
    const auto it = std::ranges::find(stamps_, id, &TerrainStamp::id);
    if (it == stamps_.end()) return bad("id: no stamp with id " + std::to_string(id));
    if (auto ok = validate_stamp(stamp); !ok) return std::unexpected(ok.error());
    stamp.id = id;
    if (*it == stamp) return {};
    *it = std::move(stamp);
    ++revision_;
    return {};
}

bool TerrainEditList::remove(std::uint64_t id) {
    const auto it = std::ranges::find(stamps_, id, &TerrainStamp::id);
    if (it == stamps_.end()) return false;
    stamps_.erase(it);
    ++revision_;
    return true;
}

void TerrainEditList::clear() {
    if (stamps_.empty()) return;
    stamps_.clear();
    ++revision_;
}

const TerrainStamp* TerrainEditList::find(std::uint64_t id) const {
    const auto it = std::ranges::find(stamps_, id, &TerrainStamp::id);
    return it == stamps_.end() ? nullptr : &*it;
}

std::expected<void, std::string> TerrainEditList::assign(std::vector<TerrainStamp> stamps,
                                                         std::uint64_t next_id) {
    if (stamps.size() > max_stamps) {
        return bad("stamps: at most " + std::to_string(max_stamps));
    }
    std::set<std::uint64_t> ids;
    std::uint64_t largest = 0;
    for (std::size_t i = 0; i < stamps.size(); ++i) {
        const std::string where = "stamps[" + std::to_string(i) + "]";
        if (auto ok = validate_stamp(stamps[i]); !ok) return bad(where + "." + ok.error());
        if (stamps[i].id == 0 || !ids.insert(stamps[i].id).second) {
            return bad(where + ".id: expected a unique id > 0");
        }
        largest = std::max(largest, stamps[i].id);
    }
    if (next_id == 0) next_id = largest + 1;
    if (next_id <= largest) {
        return bad("next_id: must be larger than every stamp id (" + std::to_string(largest) + ")");
    }
    stamps_ = std::move(stamps);
    next_id_ = next_id;
    ++revision_;
    return {};
}

std::vector<EditRect> changed_regions(std::span<const TerrainStamp> before,
                                      std::span<const TerrainStamp> after) {
    std::unordered_map<std::uint64_t, const TerrainStamp*> old_by_id;
    std::unordered_map<std::uint64_t, const TerrainStamp*> new_by_id;
    for (const TerrainStamp& s : before) old_by_id.emplace(s.id, &s);
    for (const TerrainStamp& s : after) new_by_id.emplace(s.id, &s);

    std::vector<EditRect> rects;
    for (const TerrainStamp& s : before) {
        const auto it = new_by_id.find(s.id);
        if (it == new_by_id.end() || !(*it->second == s)) rects.push_back(stamp_bounds(s));
    }
    for (const TerrainStamp& s : after) {
        const auto it = old_by_id.find(s.id);
        if (it == old_by_id.end() || !(*it->second == s)) rects.push_back(stamp_bounds(s));
    }
    // Reordering: stamps kept on both sides but in a different relative
    // order change wherever they overlap; take all of them (a rare edit).
    std::vector<std::uint64_t> old_order;
    std::vector<std::uint64_t> new_order;
    for (const TerrainStamp& s : before) {
        if (new_by_id.contains(s.id)) old_order.push_back(s.id);
    }
    for (const TerrainStamp& s : after) {
        if (old_by_id.contains(s.id)) new_order.push_back(s.id);
    }
    if (old_order != new_order) {
        for (const TerrainStamp& s : after) {
            if (old_by_id.contains(s.id)) rects.push_back(stamp_bounds(s));
        }
    }

    // Merge overlapping rectangles until none overlap.
    for (bool merged = true; merged;) {
        merged = false;
        for (std::size_t i = 0; i < rects.size() && !merged; ++i) {
            for (std::size_t j = i + 1; j < rects.size(); ++j) {
                if (rects[i].overlaps(rects[j])) {
                    rects[i] = rects[i].united(rects[j]);
                    rects.erase(rects.begin() + static_cast<std::ptrdiff_t>(j));
                    merged = true;
                    break;
                }
            }
        }
    }
    return rects;
}

} // namespace engine::terrain
