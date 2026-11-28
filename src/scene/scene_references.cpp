#include "engine/scene/scene_references.hpp"

#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <utility>

#include <entt/entity/registry.hpp>
#include <nlohmann/json.hpp>

#include "engine/core/json_fields.hpp"
#include "engine/core/utf8_path.hpp"
#include "engine/scene/component_json.hpp"
#include "engine/scene/components.hpp"

namespace engine::scene {

namespace {

using nlohmann::json;

constexpr std::uintmax_t k_max_scene_bytes = 256ULL << 20;
constexpr std::size_t    k_max_path = 4096;

[[nodiscard]] std::unexpected<core::Error> fail(std::string what) {
    return std::unexpected(core::Error{std::move(what)});
}

[[nodiscard]] std::string entity_label(std::size_t i, const json& entity) {
    std::string out = "entities[" + std::to_string(i) + "]";
    if (const auto it = entity.find("name"); it != entity.end() && it->is_string()) {
        out += " '" + it->get<std::string>().substr(0, 64) + "'";
    }
    return out;
}

// A string field of a sub-object, read leniently like the loader does.
[[nodiscard]] std::expected<std::string, core::Error>
read_path_field(const json& object, std::string_view key, const std::string& context) {
    std::string value;
    core::JsonFields f(object, context, /*strict=*/false);
    f.string(key, value, k_max_path);
    if (auto ok = f.finish(); !ok) return std::unexpected(ok.error());
    return value;
}

} // namespace

std::string_view to_string(SceneRefKind kind) {
    switch (kind) {
    case SceneRefKind::model: return "model";
    case SceneRefKind::audio: return "audio";
    case SceneRefKind::script: return "script";
    }
    return "model";
}

bool is_scene_document(const json& doc) {
    if (!doc.is_object()) return false;
    const auto it = doc.find("entities");
    return it != doc.end() && it->is_array();
}

std::expected<std::vector<SceneReference>, core::Error> scene_references(const json& root) {
    if (!is_scene_document(root)) return fail("not a scene file (no \"entities\" array)");
    const json& list = root["entities"];

    std::vector<SceneReference> refs;
    entt::registry scratch;
    for (std::size_t i = 0; i < list.size(); ++i) {
        const json& je = list[i];
        const std::string where = entity_label(i, je);
        if (!je.is_object()) return fail(where + ": expected an object");
        const entt::entity e = scratch.create();
        for (const auto& [key, value] : je.items()) {
            if (key == "name" || key == "parent") continue;
            if (key == "mesh") {
                auto gltf = read_path_field(value, "gltf", where + ".mesh");
                if (!gltf) return std::unexpected(gltf.error());
                if (!gltf->empty()) {
                    refs.push_back({SceneRefKind::model, std::move(*gltf), where + " mesh.gltf"});
                }
                continue;
            }
            // Lua scripts (G7): {"path": "<relative to assets/scripts>"}.
            // Read directly so builds without the scripting codec still ship
            // a game's scripts.
            if (key == "script") {
                auto path = read_path_field(value, "path", where + ".script");
                if (!path) return std::unexpected(path.error());
                if (!path->empty()) {
                    refs.push_back({SceneRefKind::script, std::move(*path), where + " script.path"});
                }
            }
            if (find_component_info(key) == nullptr) continue; // the loader skips these too
            // Decode exactly like the loader (lenient) so a file it would
            // refuse fails here as well, with the same message.
            if (auto ok = apply_component(scratch, e, key, value, /*strict=*/false); !ok) {
                return fail(where + ": " + ok.error().message);
            }
        }
        if (const auto* im = scratch.try_get<InstancedMesh>(e); im != nullptr && !im->gltf_path.empty()) {
            refs.push_back({SceneRefKind::model, im->gltf_path, where + " instanced_mesh.gltf"});
        }
        if (const auto* a = scratch.try_get<AudioSource>(e); a != nullptr && !a->clip.empty()) {
            refs.push_back({SceneRefKind::audio, a->clip, where + " audio_source.clip"});
        }
    }
    return refs;
}

std::expected<std::vector<SceneReference>, core::Error>
scene_file_references(const std::filesystem::path& path) {
    const std::string name = core::path_to_utf8(path);
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec) return fail("scene '" + name + "': cannot read (" + ec.message() + ")");
    if (size > k_max_scene_bytes) return fail("scene '" + name + "': file too large");
    std::ifstream in(path, std::ios::binary);
    if (!in) return fail("scene '" + name + "': cannot open");
    const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    const json doc = json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (doc.is_discarded()) return fail("scene '" + name + "': not valid JSON");
    auto refs = scene_references(doc);
    if (!refs) return fail("scene '" + name + "': " + refs.error().message);
    return refs;
}

} // namespace engine::scene
