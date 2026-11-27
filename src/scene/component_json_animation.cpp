// Component JSON codec: animator (playback + clip switching) and foot_ik.

#include "component_json_animation.hpp"

#include <string>

namespace engine::scene::detail {

namespace {

using json = nlohmann::json;

constexpr core::EnumName<animation::BlendMode> k_blend_modes[] = {
    {"inertialize", animation::BlendMode::inertialize},
    {"crossfade", animation::BlendMode::crossfade}};
constexpr std::size_t k_max_name = 256;

// A clip's codec name: its glTF name, or "#<index>" when unnamed.
[[nodiscard]] std::string clip_name(const Animator& a, std::size_t index) {
    const auto& h = a.clips[index];
    if (h.valid() && h.is_loaded() && !h->name.empty()) return h->name;
    return "#" + std::to_string(index);
}

[[nodiscard]] std::string blend_name(animation::BlendMode mode) {
    for (const auto& n : k_blend_modes) {
        if (n.value == mode) return std::string(n.name);
    }
    return "unknown";
}

} // namespace

json encode_animator(const Animator& a) {
    json out = {{"time", a.time},
                {"speed", a.speed},
                {"looping", a.looping},
                {"blend", blend_name(a.blend_mode)},
                {"blend_time", a.blend_time}};
    json names = json::array();
    for (std::size_t i = 0; i < a.clips.size(); ++i) {
        names.push_back(clip_name(a, i));
        if (a.clips[i].id() == a.clip.id()) out["clip"] = clip_name(a, i);
    }
    out["clips"] = std::move(names); // read-only: what `clip` can switch to
    return out;
}

void decode_animator(core::JsonFields& f, Animator& a) {
    std::string clip;
    f.string("clip", clip, k_max_name);
    if (!clip.empty()) {
        bool found = false;
        std::string available;
        for (std::size_t i = 0; i < a.clips.size() && !found; ++i) {
            const std::string name = clip_name(a, i);
            available += (available.empty() ? "" : ", ") + name;
            if (name != clip) continue;
            found = true;
            if (a.clips[i].id() != a.clip.id()) {
                a.clip = a.clips[i];
                a.time = 0.0F; // a new clip starts at its beginning unless `time` says otherwise
            }
        }
        if (!found && f.strict()) {
            f.fail("clip", "unknown clip '" + clip + "' (available: " +
                               (available.empty() ? "none" : available) + ")");
        }
    }
    (void)f.raw("clips");
    f.number("time", a.time, 0.0F).number("speed", a.speed, -100.0F, 100.0F);
    f.boolean("looping", a.looping);
    f.enumeration("blend", a.blend_mode, k_blend_modes);
    f.number("blend_time", a.blend_time, 0.0F, 10.0F);
}

json encode_foot_ik(const FootIK& k) {
    return {{"enabled", k.enabled},
            {"pelvis", k.pelvis},
            {"left_foot", k.left_foot},
            {"right_foot", k.right_foot},
            {"ray_up", k.ray_up},
            {"ray_down", k.ray_down},
            {"max_adjust", k.max_adjust},
            {"max_foot_angle", k.max_foot_angle},
            {"smoothing", k.smoothing}};
}

void decode_foot_ik(core::JsonFields& f, FootIK& k) {
    f.boolean("enabled", k.enabled);
    f.string("pelvis", k.pelvis, k_max_name)
        .string("left_foot", k.left_foot, k_max_name)
        .string("right_foot", k.right_foot, k_max_name);
    f.number("ray_up", k.ray_up, 0.0F, 10.0F).number("ray_down", k.ray_down, 0.0F, 10.0F);
    f.number("max_adjust", k.max_adjust, 0.0F, 2.0F);
    f.number("max_foot_angle", k.max_foot_angle, 0.0F, 90.0F);
    f.number("smoothing", k.smoothing, 0.0F, 2.0F);
}

} // namespace engine::scene::detail
