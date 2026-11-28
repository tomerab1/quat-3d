// Inspector editors for world-building components (ROADMAP E8.3): terrain,
// procedural primitives, instanced meshes and scatter. Called from the
// ComponentInspector specialisations in inspector.cpp, inside their
// property grid.

#include "inspector_world.hpp"

#define GLM_ENABLE_EXPERIMENTAL
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>

#include <entt/entity/registry.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <imgui.h>

#include "engine/rhi/material_shader_names.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/scatter.hpp"
#include "engine/terrain/budget.hpp"
#include "engine/terrain/talus_relax.hpp"
#include "terrain_material_inspector.hpp"
#include "ui/icons.hpp"
#include "ui/widgets.hpp"

namespace engine::editor {

void draw_terrain(scene::Terrain& terrain) {
    terrain::TerrainParams& p = terrain.params;
    (void)ui::prop_u32("Seed", p.seed, {.min = 0, .max = 1 << 30});
    (void)ui::prop_float("Tile size", p.tile_size_m,
                         {.speed = 10.0F, .min = 100.0F, .max = 20000.0F, .format = "%.0f m"});
    (void)ui::prop_float("Height", p.height_m,
                         {.speed = 1.0F, .min = 1.0F, .max = 2000.0F, .format = "%.0f m"});
    (void)ui::prop_int("Octaves", p.octaves, {.min = 1, .max = 10, .slider = true});
    (void)ui::prop_float("Warp", p.warp_strength,
                         {.min = 0.0F, .max = 1.0F, .format = "%.2f", .slider = true});
    static constexpr ui::EnumItem erosion[] = {
        {static_cast<int>(terrain::ErosionModel::droplets), "Droplets (CPU)", nullptr},
        {static_cast<int>(terrain::ErosionModel::grid), "Grid (GPU)", nullptr},
    };
    (void)ui::prop_enum("Erosion", p.erosion_model, erosion);
    if (p.erosion_model == terrain::ErosionModel::grid) {
        (void)ui::prop_u32("Erosion steps", p.grid.iterations,
                           {.speed = 5.0F, .min = 0, .max = 5'000});
        (void)ui::prop_u32("Talus relax", p.grid.relax_cycles,
                           {.min = 0, .max = static_cast<int>(terrain::max_talus_relax_cycles),
                            .slider = true},
                           {.reset = 3U,
                            .tooltip = "Cycles bringing slopes down to the talus angle (0 = off)"});
    } else {
        (void)ui::prop_u32("Droplets", p.erosion_droplets,
                           {.speed = 1000.0F, .min = 0, .max = 1'000'000});
    }
    // CPU and GPU synthesis produce the same heights: the choice applies to
    // the next tile generated (no regeneration).
    (void)ui::prop_bool("Generate on GPU", terrain.gpu_generate);
    (void)ui::prop_float("Snowline", terrain.snowline_m,
                         {.speed = 1.0F, .min = -1000.0F, .max = 5000.0F, .format = "%.0f m"});
    draw_terrain_material(terrain.material);
    ui::prop_separator("Streaming");
    if (ui::prop_bool("Streaming", terrain.streaming,
                      {.tooltip = "An endless seeded world streams in around the camera"})) {
        terrain.regenerate = true; // rebuild in the new mode
    }
    if (terrain.streaming) {
        // The runtime resizes the window and keeps the loaded tiles.
        (void)ui::prop_int("Radius (tiles)", terrain.stream_radius,
                           {.min = 1, .max = 4, .slider = true});
    }
    if (const auto over = terrain::terrain_budget_problem(p, terrain.streaming,
                                                          terrain.stream_radius)) {
        // #67: the runtime does not generate this configuration.
        const std::string note = "Over budget, not generated: " + over->message;
        ui::prop_note(note.c_str(), ui::tokens().warning);
    }
    ui::prop_begin("", nullptr, false, false);
    if (ui::button(ui::label(icon::refresh_cw, "Regenerate").c_str(), ui::ButtonKind::secondary,
                   ImVec2(-1.0F, 0.0F))) {
        terrain.regenerate = true;
    }
    (void)ui::prop_end(false, false);
    ui::prop_note(terrain.gpu_generate
                      ? "Tiles synthesise on the GPU (droplet erosion stays on a CPU worker); "
                        "they pop in when ready."
                      : "Generation runs on a worker thread; tiles pop in when ready.");
}

void draw_primitive(scene::Primitive& p) {
    static constexpr ui::EnumItem shapes[] = {
        {static_cast<int>(scene::PrimitiveShape::cube), "Cube", nullptr},
        {static_cast<int>(scene::PrimitiveShape::sphere), "Sphere", nullptr},
        {static_cast<int>(scene::PrimitiveShape::plane), "Plane", nullptr},
        {static_cast<int>(scene::PrimitiveShape::cylinder), "Cylinder", nullptr},
        {static_cast<int>(scene::PrimitiveShape::cone), "Cone", nullptr},
        {static_cast<int>(scene::PrimitiveShape::capsule), "Capsule", nullptr},
    };
    (void)ui::prop_enum("Shape", p.shape, shapes);
    (void)ui::prop_color4("Base colour", p.color, {.tooltip = "Linear; alpha < 1 blends"});
    (void)ui::prop_float("Metallic", p.metallic, {.min = 0.0F, .max = 1.0F, .slider = true},
                         {.reset = 0.0F});
    (void)ui::prop_float("Roughness", p.roughness, {.min = 0.0F, .max = 1.0F, .slider = true},
                         {.reset = 0.5F});
    (void)ui::prop_color3("Emissive", p.emissive, {.reset = glm::vec3(0.0F)}, /*hdr=*/true);

    // Material shader (A6): assets/shaders/materials/<name>.slang; the name
    // commits on Enter (every edit re-resolves the material).
    std::string shader = p.shader;
    if (ui::prop_text("Shader", shader,
                      {.tooltip = "Material shader under assets/shaders/materials/ (Enter to "
                                  "apply); empty = built-in PBR"},
                      /*commit_on_enter=*/true)) {
        if (shader.empty() || shader == rhi::material_shaders::builtin_name) {
            p.shader.clear();
        } else if (rhi::material_shaders::valid_name(shader)) {
            p.shader = shader;
        }
    }
    if (!p.shader.empty()) {
        (void)ui::prop_vec4("Shader params 0", p.shader_params[0], {.speed = 0.01F});
        (void)ui::prop_vec4("Shader params 1", p.shader_params[1], {.speed = 0.01F});
    }
}

void draw_instanced_mesh(entt::registry& r, entt::entity e, scene::InstancedMesh& mesh,
                         const std::filesystem::path& project_root) {
    // Paths commit on Enter: every edit re-resolves the model.
    static constexpr std::string_view exts[] = {".gltf", ".glb"};
    (void)ui::prop_asset("Model", mesh.gltf_path,
                         {.root = project_root, .extensions = exts},
                         {.tooltip = "glTF relative to the project root; empty = the "
                                     "primitive shape below"});
    if (!mesh.gltf_path.empty()) {
        (void)ui::prop_text("Node", mesh.gltf_node,
                            {.tooltip = "Only this node's subtree (empty = all)"}, true);
    } else {
        scene::Primitive& p = mesh.primitive;
        draw_primitive(p);
    }
    (void)ui::prop_bool("Cast shadows", mesh.cast_shadows, {.reset = true});

    char buf[96];
    if (const auto* parts = r.try_get<scene::InstancedMeshParts>(e)) {
        std::snprintf(buf, sizeof(buf), "%zu part(s), radius %.2f m", parts->parts.size(),
                      static_cast<double>(parts->bounds.w));
    } else {
        std::snprintf(buf, sizeof(buf), "resolving...");
    }
    ui::prop_value("Model", buf);
    if (const auto* scatter = r.try_get<scene::Scatter>(e)) {
        std::snprintf(buf, sizeof(buf), "%zu (scatter generation %u)", mesh.instances.size(),
                      scatter->generation);
        ui::prop_value("Placements", buf, "Generated by the Scatter; remove it to edit by hand");
        return;
    }
    ui::prop_separator("Placements");
    constexpr int k_listed = 64; // a long list is edited through scene files / remote API
    int remove = -1;
    const int shown = std::min(static_cast<int>(mesh.instances.size()), k_listed);
    for (int i = 0; i < shown; ++i) {
        scene::InstanceTransform& t = mesh.instances[static_cast<std::size_t>(i)];
        ImGui::PushID(i);
        char label[24];
        std::snprintf(label, sizeof(label), "#%d position", i);
        (void)ui::prop_vec3(label, t.position, {.speed = 0.05F});
        glm::vec3 euler = glm::degrees(glm::eulerAngles(t.rotation));
        if (ui::prop_vec3("rotation", euler, {.speed = 0.5F, .format = "%.1f°"})) {
            t.rotation = glm::quat(glm::radians(euler));
        }
        float uniform = t.scale.x;
        if (ui::prop_float("scale", uniform, {.speed = 0.01F, .min = 0.01F, .max = 1000.0F,
                                              .format = "%.2f"})) {
            t.scale = glm::vec3(uniform);
        }
        ui::prop_begin("", nullptr, false, false);
        if (ui::button(ui::label(icon::trash_2, "Remove").c_str(), ui::ButtonKind::danger)) {
            remove = i;
        }
        (void)ui::prop_end(false, false);
        ImGui::PopID();
    }
    if (static_cast<int>(mesh.instances.size()) > shown) {
        std::snprintf(buf, sizeof(buf), "%zu more not listed", mesh.instances.size() - shown);
        ui::prop_value("", buf);
    }
    if (remove >= 0) mesh.instances.erase(mesh.instances.begin() + remove);
    ui::prop_begin("", nullptr, false, false);
    if (ui::button(ui::label(icon::plus, "Add placement").c_str())) {
        scene::InstanceTransform t;
        if (!mesh.instances.empty()) {
            t.position = mesh.instances.back().position + glm::vec3(2.0F, 0.0F, 0.0F);
        }
        mesh.instances.push_back(t);
    }
    ImGui::SameLine();
    if (!mesh.instances.empty() && ui::button("Clear", ui::ButtonKind::ghost)) {
        mesh.instances.clear();
    }
    (void)ui::prop_end(false, false);
}

void draw_scatter(entt::registry& r, entt::entity e, scene::Scatter& s) {
    (void)ui::prop_u32("Seed", s.seed, {.min = 0, .max = 0x7FFFFFFF});
    static constexpr ui::EnumItem areas[] = {
        {static_cast<int>(scene::ScatterArea::rect), "Rectangle", nullptr},
        {static_cast<int>(scene::ScatterArea::circle), "Circle", nullptr},
    };
    (void)ui::prop_enum("Area", s.area, areas);
    if (s.area == scene::ScatterArea::rect) {
        (void)ui::prop_vec2("Size (x, z)", s.size,
                            {.speed = 0.5F, .min = 0.0F, .max = 20000.0F, .format = "%.1f m"});
    } else {
        (void)ui::prop_float("Radius", s.radius,
                             {.speed = 0.25F, .min = 0.0F, .max = 10000.0F, .format = "%.1f m"});
    }
    (void)ui::prop_float("Density", s.density,
                         {.speed = 0.001F, .min = 0.0F, .max = 100.0F, .format = "%.3f / m²"});
    (void)ui::prop_float("Min spacing", s.min_spacing,
                         {.speed = 0.05F, .min = 0.0F, .max = 1000.0F, .format = "%.2f m"});
    static constexpr ui::EnumItem patterns[] = {
        {static_cast<int>(scene::ScatterPattern::poisson), "Poisson disc", nullptr},
        {static_cast<int>(scene::ScatterPattern::jittered_grid), "Jittered grid", nullptr},
    };
    (void)ui::prop_enum("Pattern", s.pattern, patterns);
    (void)ui::prop_range("Scale", s.scale_range, {.speed = 0.01F, .min = 0.01F, .max = 100.0F});
    (void)ui::prop_bool("Random yaw", s.random_yaw);
    (void)ui::prop_float("Align to ground", s.align_to_normal,
                         {.min = 0.0F, .max = 1.0F, .slider = true});
    (void)ui::prop_float("Mirrored", s.mirror,
                         {.min = 0.0F, .max = 1.0F, .format = "%.2f", .slider = true},
                         {.tooltip = "Fraction of the placements mirrored (variety)"});
    (void)ui::prop_float("Base offset", s.base_offset,
                         {.speed = 0.01F, .min = -100.0F, .max = 100.0F, .format = "%.2f x scale"},
                         {.tooltip = "Lift along the placement's up axis, times its scale "
                                     "(0.5 seats a unit primitive on the ground)"});
    (void)ui::prop_range("Slope", s.slope_deg,
                         {.speed = 0.5F, .min = 0.0F, .max = 90.0F, .format = "%.0f°"});
    (void)ui::prop_range("Height", s.height_m,
                         {.speed = 1.0F, .min = -100000.0F, .max = 100000.0F, .format = "%.0f m"});
    (void)ui::prop_u32("Max instances", s.max_instances,
                       {.speed = 100.0F, .min = 0,
                        .max = static_cast<int>(scene::k_scatter_instance_limit)});
    const auto* mesh = r.try_get<scene::InstancedMesh>(e);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%zu (generation %u)",
                  mesh != nullptr ? mesh->instances.size() : 0, s.generation);
    ui::prop_value("Placements", buf,
                   "Regenerates when these settings, the entity's position or the terrain "
                   "change. Removing the Scatter keeps the placements.");
}

} // namespace engine::editor
