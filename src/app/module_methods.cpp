// Game-module remote methods — see module_methods.hpp.

#include "app/module_methods.hpp"

#include <algorithm>
#include <string>
#include <utility>

#include "app/module_runtime.hpp"
#include "engine/core/log.hpp"
#include "engine/game/json_schema.hpp"
#include "engine/remote/rpc_server.hpp"

namespace quat::app {

void install_module_methods(engine::remote::RpcServer& server, const ModuleRuntime& modules,
                            engine::scene::Scene& scene,
                            std::function<engine::physics::PhysicsWorld*()> physics,
                            const std::uint64_t* frame_counter) {
    namespace remote = engine::remote;
    const engine::game::ModuleSet* set = modules.modules();
    if (set == nullptr) return;
    const std::vector<remote::MethodInfo> existing = server.methods();
    for (const engine::game::MethodDesc& m : set->methods()) {
        if (std::ranges::any_of(existing, [&](const remote::MethodInfo& e) { return e.name == m.name; })) {
            engine::core::log(engine::core::LogLevel::warn, "game",
                              "module method '{}' is already defined; skipped", m.name);
            continue;
        }
        // `m` lives in the ModuleSet, which outlives the endpoint.
        server.add_method(m.name, m.summary,
                          [&m, &modules, &scene, physics, frame_counter](
                              const remote::Json& params) -> remote::RpcResult {
                              if (auto ok = engine::game::validate_json(m.params_schema, params); !ok) {
                                  return remote::rpc_fail(remote::rpc_code::invalid_params,
                                                          ok.error().message);
                              }
                              engine::game::Services services = modules.services(physics ? physics() : nullptr);
                              engine::game::SystemContext context(
                                  scene, services, frame_counter != nullptr ? *frame_counter : 0);
                              auto result = m.handler(context, params);
                              if (!result) {
                                  return remote::rpc_fail(remote::rpc_code::failed, result.error().message);
                              }
                              return std::move(*result);
                          });
        (void)server.set_method_schemas(m.name, m.params_schema, m.result_schema);
    }
}

} // namespace quat::app
