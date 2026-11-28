// Component JSON codec — see include/engine/scene/component_json.hpp.
//
// Dispatch only: every component's encoder / decoder lives in its own file
// under src/scene/codec/ and reaches these functions through the component
// registry (component_registry.hpp, ROADMAP S9).

#include "engine/scene/component_json.hpp"

#include <algorithm>
#include <string>

#include <entt/entity/registry.hpp>
#include <nlohmann/json.hpp>

#include "engine/scene/component_registry.hpp"

namespace engine::scene {

namespace {

using json = nlohmann::json;

[[nodiscard]] bool wanted(const EncodeOptions& options, std::string_view name) {
    return options.only.empty() || std::ranges::find(options.only, name) != options.only.end();
}

} // namespace

std::span<const ComponentInfo> component_infos() { return component_registry().infos(); }

const ComponentInfo* find_component_info(std::string_view name) {
    for (const ComponentInfo& c : component_infos()) {
        if (c.name == name) return &c;
    }
    return nullptr;
}

json encode_components(const entt::registry& r, entt::entity e, const EncodeOptions& options) {
    json out = json::object();
    const ComponentRegistry& registry = component_registry();
    for (std::size_t i = 0; i < registry.size(); ++i) {
        const ComponentCodec& c = registry.at(i);
        if (!c.writable && !options.include_read_only) continue;
        if (!wanted(options, c.name) || !c.has(r, e)) continue;
        out[c.name] = c.encode(r, e, options);
    }
    return out;
}

std::vector<std::string_view> component_names(const entt::registry& r, entt::entity e,
                                              bool include_read_only) {
    std::vector<std::string_view> out;
    if (!r.valid(e)) return out;
    const ComponentRegistry& registry = component_registry();
    for (std::size_t i = 0; i < registry.size(); ++i) {
        const ComponentCodec& c = registry.at(i);
        if ((c.writable || include_read_only) && c.has(r, e)) out.push_back(c.name);
    }
    return out;
}

std::expected<void, core::Error> apply_component(entt::registry& r, entt::entity e,
                                                 std::string_view name, const json& v,
                                                 bool strict) {
    if (!r.valid(e)) return std::unexpected(core::Error{"entity does not exist"});
    const ComponentCodec* codec = component_registry().find(name);
    if (codec == nullptr) {
        std::string known;
        for (const ComponentInfo& c : component_infos()) {
            if (c.writable) known += (known.empty() ? "" : ", ") + std::string(c.name);
        }
        return std::unexpected(
            core::Error{"unknown component '" + std::string(name) + "' (known: " + known + ")"});
    }
    if (!codec->writable) {
        return std::unexpected(core::Error{"component '" + std::string(name) + "' is read-only"});
    }
    return codec->apply(r, e, v, strict);
}

bool remove_component(entt::registry& r, entt::entity e, std::string_view name) {
    if (!r.valid(e)) return false;
    const ComponentCodec* codec = component_registry().find(name);
    if (codec == nullptr || !codec->remove) return false; // unknown, or transform
    return codec->remove(r, e);
}

} // namespace engine::scene
