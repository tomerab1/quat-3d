#include "shadow_ao_panel.hpp"

#include <cstdint>

#include <imgui.h>

#include "engine/editor/editor.hpp"
#include "engine/renderer/shadow_ao_settings.hpp"

namespace engine::editor {

namespace {

void slider(const char* label, float& v, renderer::FloatRange range, const char* format) {
    ImGui::SliderFloat(label, &v, range.min, range.max, format);
}

void slider(const char* label, std::uint32_t& v, renderer::UintRange range) {
    int i = static_cast<int>(v);
    if (ImGui::SliderInt(label, &i, static_cast<int>(range.min), static_cast<int>(range.max))) {
        v = static_cast<std::uint32_t>(i);
    }
}

} // namespace

void draw_shadow_ao_settings(RendererSettings& settings) {
    using renderer::ContactShadowSettings;
    using renderer::CsmSettings;
    using renderer::GtaoSettings;

    if (ImGui::CollapsingHeader("Shadows")) {
        ImGui::PushID("csm");
        CsmSettings& c = settings.csm;
        ImGui::Checkbox("sun shadows", &c.enabled);
        ImGui::Checkbox("show cascades", &c.debug_cascades);
        slider("cascades", c.cascade_count, CsmSettings::k_cascade_count);
        slider("distance", c.max_distance, CsmSettings::k_max_distance, "%.0f m");
        slider("split lambda", c.split_lambda, CsmSettings::k_split_lambda, "%.2f");
        ImGui::Checkbox("fit to depth (GPU)", &c.gpu_depth_bounds);
        slider("blend band", c.blend_band, CsmSettings::k_blend_band, "%.2f");
        slider("PCF radius", c.pcf_radius, CsmSettings::k_pcf_radius, "%.1f texels");
        slider("sun size (PCSS)", c.light_size_deg, CsmSettings::k_light_size_deg, "%.2f deg");
        slider("normal bias", c.normal_bias, CsmSettings::k_normal_bias, "%.2f texels");
        slider("depth bias", c.depth_bias, CsmSettings::k_depth_bias, "%.2f texels");
        slider("caster reach", c.caster_distance, CsmSettings::k_caster_distance, "%.0f m");
        ImGui::PopID();

        ImGui::PushID("contact");
        ContactShadowSettings& k = settings.contact_shadows;
        ImGui::SeparatorText("Contact shadows");
        ImGui::Checkbox("enabled", &k.enabled);
        slider("length", k.length, ContactShadowSettings::k_length, "%.2f m");
        slider("steps", k.steps, ContactShadowSettings::k_steps);
        slider("thickness", k.thickness, ContactShadowSettings::k_thickness, "%.2f m");
        slider("intensity", k.intensity, ContactShadowSettings::k_intensity, "%.2f");
        ImGui::PopID();
    }
    if (ImGui::CollapsingHeader("Ambient occlusion")) {
        ImGui::PushID("gtao");
        GtaoSettings& a = settings.gtao;
        ImGui::Checkbox("GTAO", &a.enabled);
        slider("radius", a.radius, GtaoSettings::k_radius, "%.2f m");
        slider("strength (power)", a.final_value_power, GtaoSettings::k_final_value_power,
               "%.2f");
        slider("falloff", a.falloff_range, GtaoSettings::k_falloff_range, "%.2f");
        slider("slices", a.slice_count, GtaoSettings::k_slice_count);
        slider("steps per slice", a.steps_per_slice, GtaoSettings::k_steps_per_slice);
        slider("denoise passes", a.denoise_passes, GtaoSettings::k_denoise_passes);
        slider("thin occluders", a.thin_occluder_compensation,
               GtaoSettings::k_thin_occluder_compensation, "%.2f");
        ImGui::Checkbox("bent normals", &a.bent_normals);
        ImGui::Checkbox("specular occlusion", &a.specular_occlusion);
        ImGui::Checkbox("multi-bounce", &a.multi_bounce);
        ImGui::PopID();
    }
}

} // namespace engine::editor
