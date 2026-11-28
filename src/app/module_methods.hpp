#pragma once

// Game-module remote methods (ROADMAP G17.1) on the application's endpoint.
// Not compiled into the shipping player (no remote control there).

#include <cstdint>
#include <functional>

namespace engine::physics {
class PhysicsWorld;
}
namespace engine::remote {
class RpcServer;
}
namespace engine::scene {
class Scene;
}

namespace quat::app {

class ModuleRuntime;

// Adds each module method to `server`: parameters are validated against the
// method's JSON Schema (invalid_params with the path on failure), the handler
// runs with the scene and services, and rpc.methods lists the schemas. A
// method whose name the server already has is skipped with a warning (the
// Registrar keeps games out of the engine's namespaces already).
void install_module_methods(engine::remote::RpcServer& server, const ModuleRuntime& modules,
                            engine::scene::Scene& scene,
                            std::function<engine::physics::PhysicsWorld*()> physics,
                            const std::uint64_t* frame_counter);

} // namespace quat::app
