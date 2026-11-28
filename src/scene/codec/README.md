# Codec components

One file per component of the scene's JSON codec
(`include/engine/scene/component_json.hpp`). Scene files, the remote API,
Lua scripts and the editor all read and write components through it.

## Adding a component

Add `src/scene/codec/<name>.cpp`, which defines

```cpp
namespace engine::scene::codec {
std::expected<void, core::Error> register_<name>(ComponentRegistry& registry);
}
```

and registers a `ComponentCodec` whose `name` is `<name>`. The build finds
the file (a `CONFIGURE_DEPENDS` glob) and generates the call to it
(`engine_components.cpp.in`, `cmake/QuatRegistration.cmake`). Do not edit
any other list. The `EngineComponents` tests check that every file in this
directory registers the component it is named after.

Shared helpers are in `src/scene/component_json_internal.hpp`, for example
`field_codec<T>`, `merge<T>`, colour, range and path readers, and physics
invalidation. Larger field codecs that other code also uses stay in their
own files and are called from here, for example `particle_json.cpp`,
`weather_json.cpp` and `world_clock_json.cpp`.

## Canonical order

`ComponentCodec::order` sets the order in which scene files, scripts and
listings visit components. A component that refers to another one comes
after it: a scatter after its instanced mesh, and a joint after the bodies.
Engine components use steps of 100:

| order | components |
|---|---|
| 100–800 | transform, primitive, material, instanced_mesh, scatter, point_light, dir_light, camera |
| 900–1200 | collider, rigidbody, character, joint |
| 1300–1600 | nav_agent, behavior, patrol, script |
| 1700–2200 | terrain, grass, weather, particle_emitter, world_clock, net_replicated |
| 2300–2800 | audio_source, audio_listener, animator, foot_ik, camera_rig, camera_rig_input |
| 2900 | mesh (read-only) |

To place a new engine component between two others, give it an unused value
between theirs. Game components (G17.1) default to
`ComponentCodec::k_default_order`, which comes after every engine component.
