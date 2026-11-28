#pragma once

// Who owns the scene's sun (issue #121). Three things can position the
// directional light the frame is lit with, and exactly one does at a time:
//
//   hdri    the loaded HDRI's own sun (sky source "hdri" with `hdri_sun` on):
//           the renderer lights with the panorama's sun, placed by the
//           environment's rotation and scaled by its intensity (R21),
//           instead of the scene's DirectionalLight;
//   clock   the enabled world clock with `drive_sun` (R22.1) writes the
//           first DirectionalLight from the apparent Sun each tick;
//   manual  otherwise: the first DirectionalLight as the scene set it.
//
// The HDRI outranks the clock. The clock keeps running underneath — its
// SkyState (stars, moon) advances and it keeps the DirectionalLight current —
// but that light does not reach the frame; turning the HDRI off hands the
// sun back to the clock without a jump.
//
// The frame loop (FrameRenderer::gather), environment.get / environment.set
// and the editor's Renderer panel all decide through these functions, so
// the light that is rendered and the one that is reported cannot disagree.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include <entt/entity/fwd.hpp>

#include "engine/scene/components.hpp"

namespace engine::renderer {
struct EnvironmentSettings;
struct HdriSun;
} // namespace engine::renderer

namespace engine::scene {

enum class SunSource : std::uint8_t { manual, clock, hdri };
inline constexpr std::array<std::string_view, 3> sun_source_names{"manual", "clock", "hdri"};

[[nodiscard]] constexpr std::string_view sun_source_name(SunSource source) {
    return sun_source_names[static_cast<std::size_t>(source)];
}

// The owner of the sun. `hdri_loaded`: an HDRI panorama is loaded (with or
// without a sun — a sunless HDRI still owns the light, and puts it out).
[[nodiscard]] SunSource sun_source(const entt::registry& registry,
                                   const renderer::EnvironmentSettings& environment,
                                   bool hdri_loaded);

// The sun the frame is lit with.
struct SceneSun {
    SunSource source = SunSource::manual;
    // As the DirectionalLight component: `direction` is the light's travel
    // direction. nullopt: no directional light (a scene without a
    // DirectionalLight that the HDRI does not light).
    std::optional<DirectionalLight> light;
};

// `hdri`: the loaded panorama's sun analysis, or null while none is loaded.
[[nodiscard]] SceneSun resolve_sun(const entt::registry& registry,
                                   const renderer::EnvironmentSettings& environment,
                                   const renderer::HdriSun* hdri);

} // namespace engine::scene
