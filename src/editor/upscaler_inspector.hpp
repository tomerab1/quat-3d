#pragma once

// Renderer-panel section for the temporal anti-aliasing / upscaling settings
// (R11). Private editor header; draw_renderer_panel delegates here so
// inspector.cpp stays within the size rule.

namespace engine::renderer {
struct UpscalerSettings;
} // namespace engine::renderer

namespace engine::editor {

// Mode, render-scale override, sharpness and the active backend.
void draw_upscaler_settings(renderer::UpscalerSettings& settings);

} // namespace engine::editor
