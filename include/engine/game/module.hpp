#pragma once

// Native game modules (ROADMAP G17.1; design: docs/design/cozy-builder.md
// §3.6, guide: docs/game-modules.md).
//
// A game module is a static C++ library with one registration function
// (RegisterFn). quat_add_game() (Quat3dGame.cmake) links the modules into the
// game's player and editor and generates the call to each function at boot;
// there are no plugin DLLs and no static-initialiser tricks. The function
// receives a Registrar and declares, through it:
//   - components, as field tables (fields.hpp): codec, remote schema, Lua
//     access and inspector rows, without codec or UI code;
//   - stateless systems in named phases of the frame, ordered with `after`;
//   - remote methods with JSON Schemas for their parameters;
//   - Lua modules (engine/script/module_bind.hpp);
//   - G5 input action maps (JSON files in the project).
// UI screens (G6.1) and save chunks (G11.1) are declared here too; this engine
// version has neither, and registering one fails the boot with a message
// naming the ROADMAP item.
//
// Registrar calls never fail on the spot: problems (a duplicate name, an
// unknown `after`, an invalid schema) are collected and reported together when
// the player boots, naming the module and the item.

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <entt/entity/registry.hpp>
#include <nlohmann/json.hpp>

#include "engine/core/error.hpp"
#include "engine/core/json_fields.hpp"
#include "engine/game/fields.hpp"
#include "engine/scene/component_registry.hpp"
#include "engine/script/module_bind.hpp"

namespace engine::physics {
class PhysicsWorld;
}
namespace engine::input {
class Actions;
}
namespace engine::scene {
class Scene;
}

namespace engine::game {

// Where module systems run in the frame, in this order.
enum class Phase : std::uint8_t {
    input,          // after input sampling (G5), before scripts and physics; once per frame
    pre_physics,    // gameplay that drives physics; once per physics slice
    post_physics,   // after the physics and character systems; once per physics slice
    post_transform, // after world transforms resolve: generators, scatter; once per frame
    pre_render,     // the last chance before render collection; once per frame
};
inline constexpr std::size_t k_phase_count = 5;
[[nodiscard]] std::string_view phase_name(Phase phase);

// What the application owns that modules may use. Each is null when the
// running configuration lacks it (no physics world outside play mode, ...).
// The job pool, command stack (E1), terrain edits (G19), dynamic meshes
// (R23.2) and seasons (R25) join as their items land. The world clock and
// weather are scene components (engine/scene/world_clock.hpp,
// engine/weather/weather_state.hpp), read through the registry.
struct Services {
    physics::PhysicsWorld*  physics = nullptr; // the running physics world (queries, bodies)
    input::Actions*         actions = nullptr; // G5 actions, evaluated this frame
    std::filesystem::path   project_root{};    // the game project (holds game.json)
};

// Handed to systems and method handlers: the world they act on.
class SystemContext {
public:
    SystemContext(scene::Scene& scene, Services& services, std::uint64_t frame = 0)
        : scene_(&scene), services_(&services), frame_(frame) {}

    [[nodiscard]] scene::Scene&    scene() { return *scene_; }
    [[nodiscard]] entt::registry&  registry();
    [[nodiscard]] Services&        services() { return *services_; }
    [[nodiscard]] std::uint64_t    frame() const { return frame_; }

private:
    scene::Scene* scene_;
    Services*     services_;
    std::uint64_t frame_;
};

// A system: a stateless function. State lives in components (singleton
// components included) or game-owned stores the system reaches through them.
using SystemFn = void (*)(SystemContext& context, float dt);

struct SystemDesc {
    std::string_view name;  // unique, e.g. "cozy.generate" (profiling, logs, `after`)
    Phase            phase = Phase::pre_physics;
    SystemFn         run = nullptr;
    // Systems of the same phase this one runs after.
    std::span<const std::string_view> after{};
};

// A remote method (the JSON-RPC endpoint, docs/remote-control.md). `params`
// arrives validated against `params_schema`. The method's namespace (the part
// before the first '.') may not be one of the engine's.
using MethodFn = std::function<std::expected<nlohmann::json, core::Error>(
    SystemContext& context, const nlohmann::json& params)>;

struct MethodDesc {
    std::string    name{};    // "<namespace>.<verb>", lowercase, e.g. "build.place"
    std::string    summary{}; // one line, for rpc.methods
    nlohmann::json params_schema = nlohmann::json::object(); // JSON Schema (supported subset: docs/game-modules.md)
    nlohmann::json result_schema = nlohmann::json::object(); // documentation only
    MethodFn       handler{};
};

// G6.1 / G11.1 placeholders: the shapes the design names. Registering one
// fails the boot until those items land.
struct UiScreenDesc {
    std::string name{};
    std::string layout_path{}; // project-relative
};
struct SaveChunkDesc {
    std::string   name{};
    std::uint32_t version = 1;
};

class Registrar {
public:
    // `components`: where components go (the process-wide registry in a game).
    Registrar(std::string module_name, scene::ComponentRegistry& components);
    ~Registrar();
    Registrar(const Registrar&) = delete;
    Registrar& operator=(const Registrar&) = delete;

    [[nodiscard]] const std::string& module_name() const { return module_; }

    // A component described by a field table (fields.hpp). Its name is the
    // JSON key in scene files and the remote API; prefix it with the game's
    // name ("cozy_wall") so it never collides with an engine component.
    template <typename T>
    void component(std::string_view name, std::string_view summary, const ComponentFields<T>& fields);
    void system(const SystemDesc& desc);
    void method(MethodDesc desc);
    void lua_module(script::LuaModuleDesc desc);
    // A G5 action map (JSON, docs/input.md) in the project, merged into the
    // game's map; its contexts and actions may not clash with existing ones.
    void input_actions(std::string_view project_path);
    void ui_screen(UiScreenDesc desc);   // needs G6.1
    void save_chunk(SaveChunkDesc desc); // needs G11.1

    // Records a problem against this module (also for the module's own checks).
    void fail(std::string message);

    struct Impl;
    [[nodiscard]] Impl& impl() { return *impl_; }

private:
    void add_component(scene::ComponentCodec codec);

    std::string           module_;
    std::unique_ptr<Impl> impl_;
};

// The function each module provides (quat_add_game_module(... REGISTER fn)).
using RegisterFn = std::expected<void, core::Error> (*)(Registrar& registrar);

// A module as the generated player entry point lists it: its name (the one
// game.json's "modules" names) and its registration function.
struct ModuleEntry {
    std::string_view name;
    RegisterFn       register_fn = nullptr;
};

template <typename T>
void Registrar::component(std::string_view name, std::string_view summary,
                          const ComponentFields<T>& fields) {
    std::shared_ptr<const scene::FieldTable> table = fields.table();
    scene::ComponentCodec codec{.name = std::string(name), .summary = std::string(summary)};
    codec.encode = [table](const entt::registry& r, entt::entity e, const scene::EncodeOptions&) {
        return scene::encode_field_table(*table, &r.get<T>(e));
    };
    codec.apply = [table, key = std::string(name)](entt::registry& r, entt::entity e,
                                                   const nlohmann::json& v, bool strict)
        -> std::expected<void, core::Error> {
        T component = r.all_of<T>(e) ? r.get<T>(e) : T{};
        core::JsonFields f(v, key, strict);
        scene::decode_field_table(f, *table, &component);
        if (auto ok = f.finish(); !ok) return std::unexpected(ok.error());
        r.emplace_or_replace<T>(e, std::move(component));
        return {};
    };
    codec.schema = std::make_shared<const nlohmann::json>(scene::field_table_schema(*table, name));
    codec.fields = std::move(table);
    add_component(scene::typed_codec<T>(std::move(codec)));
}

} // namespace engine::game
