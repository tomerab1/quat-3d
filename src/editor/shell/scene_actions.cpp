#include "scene_actions.hpp"

#include <array>
#include <cstdio>
#include <string>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>
#include <nlohmann/json.hpp>

#include "../ui/icons.hpp"
#include "engine/scene/component_json.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/scene.hpp"

namespace engine::editor::shell {

namespace {

constexpr std::array k_kinds = {
    CreateKind{"empty", "Empty Entity", "", icon::box, "entity", "", "{}"},
    CreateKind{"cube", "Cube", "Shapes", icon::cuboid, "cube", "primitive", R"({"shape":"cube"})"},
    CreateKind{"sphere", "Sphere", "Shapes", icon::circle_dot, "sphere", "primitive",
               R"({"shape":"sphere"})"},
    CreateKind{"plane", "Plane", "Shapes", icon::layers, "plane", "primitive",
               R"({"shape":"plane"})"},
    CreateKind{"cylinder", "Cylinder", "Shapes", icon::cylinder, "cylinder",
               "primitive", R"({"shape":"cylinder"})"},
    CreateKind{"capsule", "Capsule", "Shapes", icon::pill, "capsule", "primitive",
               R"({"shape":"capsule"})"},
    CreateKind{"point_light", "Point Light", "Lights", icon::lightbulb, "point_light",
               "point_light", "{}"},
    CreateKind{"dir_light", "Directional Light", "Lights", icon::sun, "sun", "dir_light", "{}"},
    CreateKind{"camera", "Camera", "Other", icon::camera, "camera", "camera", "{}"},
    CreateKind{"audio_source", "Audio Source", "Other", icon::volume_2, "audio_source",
               "audio_source", "{}"},
    CreateKind{"terrain", "Terrain", "Other", icon::mountain, "terrain", "terrain", "{}"},
};

void collect_subtree(const entt::registry& r, entt::entity root, std::vector<entt::entity>& out) {
    out.push_back(root);
    if (const auto* children = r.try_get<scene::Children>(root)) {
        for (const entt::entity child : children->entities) collect_subtree(r, child, out);
    }
}

} // namespace

std::span<const CreateKind> create_kinds() { return k_kinds; }

entt::entity create_entity_of(scene::Scene& scene, const CreateKind& kind, entt::entity parent,
                              const glm::vec3& at) {
    entt::registry& r = scene.registry();
    const entt::entity e = scene.create_entity(std::string(kind.name));
    if (!kind.component.empty()) {
        const nlohmann::json params =
            nlohmann::json::parse(kind.params, nullptr, /*allow_exceptions=*/false);
        if (auto ok = scene::apply_component(r, e, kind.component, params, /*strict=*/false);
            !ok) {
            std::fprintf(stderr, "[editor] create %s failed: %s\n", std::string(kind.id).c_str(),
                         ok.error().message.c_str());
            r.destroy(e);
            return entt::null;
        }
    }
    glm::vec3 local = at;
    if (parent != entt::null && r.valid(parent) && scene.set_parent(e, parent)) {
        if (const auto* pt = r.try_get<scene::Transform>(parent)) {
            local = glm::vec3(glm::inverse(pt->world) * glm::vec4(at, 1.0F));
        }
    }
    auto& t = r.get<scene::Transform>(e);
    t.local = glm::translate(glm::mat4(1.0F), local) * t.local;
    return e;
}

void delete_entity_tree(scene::Scene& scene, entt::entity root, entt::entity& selected) {
    entt::registry& r = scene.registry();
    if (!r.valid(root)) return;
    // Unlink from the parent first so its Children list stays sane.
    scene.set_parent(root, entt::null);
    std::vector<entt::entity> doomed;
    collect_subtree(r, root, doomed);
    for (const entt::entity e : doomed) {
        if (e == selected) selected = entt::null;
        if (r.valid(e)) r.destroy(e);
    }
}

glm::vec3 spawn_point(const glm::mat4& view, float distance) {
    const glm::mat4 cam = glm::inverse(view);
    const glm::vec3 pos(cam[3]);
    const glm::vec3 forward = -glm::normalize(glm::vec3(cam[2]));
    return pos + forward * distance;
}

} // namespace engine::editor::shell
