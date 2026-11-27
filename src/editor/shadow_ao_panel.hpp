#pragma once

// The Renderer panel's "Shadows" and "Ambient occlusion" sections (R12):
// sliders over CsmSettings, ContactShadowSettings and GtaoSettings, bounded
// by each field's declared range.

namespace engine::editor {

struct RendererSettings;

void draw_shadow_ao_settings(RendererSettings& settings);

} // namespace engine::editor
