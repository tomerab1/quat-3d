// Widget kit: the property grid and its typed rows.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include <glm/gtc/constants.hpp>
#include <imgui.h>
#include <imgui_internal.h> // RenderTextEllipsis
#include <imgui_stdlib.h>

#include "fonts.hpp"
#include "icons.hpp"
#include "metrics.hpp"
#include "widgets.hpp"

namespace engine::editor::ui {

namespace {

// A slider whose grab is replaced by a fill bar under the value text: the
// frame and the fill go to draw channel 0, ImGui's slider (transparent frame
// and grab, so only its text and border show) to channel 1 above them.
template <typename Fn>
bool fill_slider(float fraction, Fn&& slider) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImDrawListSplitter splitter;
    splitter.Split(dl, 2);
    splitter.SetCurrentChannel(dl, 1);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, IM_COL32(0, 0, 0, 0));
    const bool changed = slider();
    ImGui::PopStyleColor(5);
    splitter.SetCurrentChannel(dl, 0);
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    const float rounding = ImGui::GetStyle().FrameRounding;
    const ImGuiCol bg = ImGui::IsItemActive()    ? ImGuiCol_FrameBgActive
                        : ImGui::IsItemHovered() ? ImGuiCol_FrameBgHovered
                                                 : ImGuiCol_FrameBg;
    dl->AddRectFilled(min, max, ImGui::GetColorU32(bg), rounding);
    const float t = std::isfinite(fraction) ? std::clamp(fraction, 0.0F, 1.0F) : 0.0F;
    if (t > 0.0F) {
        const ImVec2 fill_max(min.x + (max.x - min.x) * t, max.y);
        const Theme& tk = tokens();
        dl->AddRectFilled(min, fill_max,
                          to_u32(with_alpha(tk.accent, ImGui::IsItemActive() ? 0.55F : 0.40F)),
                          rounding,
                          t >= 1.0F ? ImDrawFlags_RoundCornersAll : ImDrawFlags_RoundCornersLeft);
    }
    splitter.Merge(dl);
    return changed;
}

// Where `v` sits in [min, max], on the slider's own (linear or log) scale.
[[nodiscard]] float slider_fraction(float v, float min, float max, bool logarithmic) {
    if (!(max > min)) return 0.0F;
    if (logarithmic && min > 0.0F && v > 0.0F) {
        return std::log(v / min) / std::log(max / min);
    }
    return (v - min) / (max - min);
}

// Width of the reset arrow (square, row height).
[[nodiscard]] float reset_width() { return ImGui::GetFrameHeight(); }

template <typename T>
[[nodiscard]] bool differs(const std::optional<T>& reset, const T& v) {
    return reset.has_value() && !(*reset == v);
}

// One axis segment: a drag field with the axis colour stripe on its left.
bool axis_drag(const char* id, float& v, float width, Rgba axis, const char* axis_name,
               const DragSpec& spec) {
    ImGui::SetNextItemWidth(width);
    const bool changed = ImGui::DragFloat(id, &v, spec.speed, spec.min, spec.max, spec.format);
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    ImGui::GetWindowDrawList()->AddRectFilled(
        min, ImVec2(min.x + scaled(Sizes::axis_marker), max.y), to_u32(axis),
        ImGui::GetStyle().FrameRounding, ImDrawFlags_RoundCornersLeft);
    item_tooltip(axis_name);
    return changed;
}

// N axis segments sharing the current item width.
bool axis_fields(float* values, int count, const DragSpec& spec) {
    const Theme& t = tokens();
    const Rgba axes[4] = {t.axis_x, t.axis_y, t.axis_z, t.text_disabled};
    const char* names[4] = {"X", "Y", "Z", "W"};
    const char* ids[4] = {"##x", "##y", "##z", "##w"};
    const float spacing = scaled(Spacing::xxs);
    const float total = ImGui::CalcItemWidth();
    const float each = std::floor((total - spacing * static_cast<float>(count - 1)) /
                                  static_cast<float>(count));
    bool changed = false;
    ImGui::BeginGroup();
    for (int i = 0; i < count; ++i) {
        if (i > 0) ImGui::SameLine(0.0F, spacing);
        const float w = i + 1 == count ? total - (each + spacing) * static_cast<float>(count - 1)
                                       : each;
        changed |= axis_drag(ids[i], values[i], w, axes[i], names[i], spec);
    }
    ImGui::EndGroup();
    return changed;
}

} // namespace

// ---- Grid ------------------------------------------------------------------

bool begin_property_grid(const char* id) {
    const ImGuiTableFlags flags = ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoSavedSettings |
                                  ImGuiTableFlags_PadOuterX;
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(scaled(Spacing::xxs), scaled(2.0F)));
    if (!ImGui::BeginTable(id, 2, flags)) {
        ImGui::PopStyleVar();
        return false;
    }
    const float label_w = label_column_width(ImGui::GetContentRegionAvail().x, ui_scale());
    ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed, label_w);
    ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
    return true;
}

void end_property_grid() {
    ImGui::EndTable();
    ImGui::PopStyleVar();
}

void prop_begin(const char* text, const char* tooltip, bool has_reset, bool modified) {
    const Theme& t = tokens();
    ImGui::TableNextRow(ImGuiTableRowFlags_None, ImGui::GetFrameHeight());
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    // Modified values get primary-weight labels, defaults stay secondary.
    const Rgba col = modified ? t.text : t.text_secondary;
    ImGui::PushStyleColor(ImGuiCol_Text, to_imvec4(col));
    const float avail = ImGui::GetContentRegionAvail().x;
    const ImVec2 size = ImGui::CalcTextSize(text);
    if (size.x > avail) {
        // Clip long labels with an ellipsis; the tooltip shows the full name.
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::RenderTextEllipsis(ImGui::GetWindowDrawList(), p,
                                  ImVec2(p.x + avail, p.y + ImGui::GetFrameHeight()), p.x + avail,
                                  text, nullptr, &size);
        ImGui::Dummy(ImVec2(avail, size.y));
    } else {
        ImGui::TextUnformatted(text);
    }
    ImGui::PopStyleColor();
    if (tooltip != nullptr) {
        item_tooltip(tooltip);
    } else if (size.x > avail) {
        item_tooltip(text);
    }
    ImGui::TableSetColumnIndex(1);
    ImGui::PushID(text);
    const float w = ImGui::GetContentRegionAvail().x -
                    (has_reset ? reset_width() + ImGui::GetStyle().ItemInnerSpacing.x : 0.0F);
    ImGui::SetNextItemWidth(std::max(w, scaled(40.0F)));
}

bool prop_end(bool has_reset, bool modified) {
    bool clicked = false;
    if (has_reset && modified) {
        ImGui::SameLine(0.0F, ImGui::GetStyle().ItemInnerSpacing.x);
        // Same square, ink-centred icon button as the toolbars.
        ImGui::PushStyleColor(ImGuiCol_Text, to_imvec4(tokens().accent_text));
        clicked = icon_button("reset", icon::rotate_ccw, "Reset to default");
        ImGui::PopStyleColor();
    }
    ImGui::PopID();
    return clicked;
}

// Wraps one typed row: label, widget (fn), reset arrow.
template <typename T, typename Fn>
bool prop_row(const char* text, T& v, const PropOpts<T>& opts, Fn&& fn) {
    const bool has_reset = opts.reset.has_value() && !opts.read_only;
    const bool modified = differs(opts.reset, v);
    prop_begin(text, opts.tooltip, has_reset, modified);
    ImGui::BeginDisabled(opts.read_only);
    bool changed = fn();
    ImGui::EndDisabled();
    if (prop_end(has_reset, modified)) {
        v = *opts.reset;
        changed = true;
    }
    return changed;
}

// ---- Typed rows ------------------------------------------------------------

bool prop_float(const char* text, float& v, const DragSpec& spec, const PropOpts<float>& opts) {
    return prop_row(text, v, opts, [&] {
        if (spec.slider && spec.min < spec.max) {
            return slider_float("##v", &v, spec.min, spec.max, spec.format);
        }
        return ImGui::DragFloat("##v", &v, spec.speed, spec.min, spec.max, spec.format,
                                spec.clamp ? ImGuiSliderFlags_AlwaysClamp : 0);
    });
}

bool prop_angle(const char* text, float& radians, const DragSpec& spec,
                const PropOpts<float>& opts) {
    float degrees = glm::degrees(radians);
    PropOpts<float> deg_opts;
    deg_opts.tooltip = opts.tooltip;
    deg_opts.read_only = opts.read_only;
    if (opts.reset) deg_opts.reset = glm::degrees(*opts.reset);
    const bool changed = prop_float(text, degrees, spec, deg_opts);
    if (changed) radians = glm::radians(degrees);
    return changed;
}

bool prop_int(const char* text, int& v, const IntSpec& spec, const PropOpts<int>& opts) {
    return prop_row(text, v, opts, [&] {
        if (spec.slider && spec.min < spec.max) {
            return slider_int("##v", &v, spec.min, spec.max, spec.format);
        }
        return ImGui::DragInt("##v", &v, spec.speed, spec.min, spec.max, spec.format);
    });
}

bool prop_u32(const char* text, std::uint32_t& v, const IntSpec& spec,
              const PropOpts<std::uint32_t>& opts) {
    return prop_row(text, v, opts, [&] {
        const std::uint32_t lo = static_cast<std::uint32_t>(std::max(spec.min, 0));
        const std::uint32_t hi = static_cast<std::uint32_t>(std::max(spec.max, 0));
        return ImGui::DragScalar("##v", ImGuiDataType_U32, &v, spec.speed,
                                 spec.min < spec.max ? &lo : nullptr,
                                 spec.min < spec.max ? &hi : nullptr, "%u");
    });
}

bool prop_bool(const char* text, bool& v, const PropOpts<bool>& opts) {
    return prop_row(text, v, opts, [&] { return ImGui::Checkbox("##v", &v); });
}

bool prop_vec2(const char* text, glm::vec2& v, const DragSpec& spec,
               const PropOpts<glm::vec2>& opts) {
    return prop_row(text, v, opts, [&] { return axis_fields(&v.x, 2, spec); });
}

bool prop_vec3(const char* text, glm::vec3& v, const DragSpec& spec,
               const PropOpts<glm::vec3>& opts) {
    return prop_row(text, v, opts, [&] { return axis_fields(&v.x, 3, spec); });
}

bool prop_vec4(const char* text, glm::vec4& v, const DragSpec& spec,
               const PropOpts<glm::vec4>& opts) {
    return prop_row(text, v, opts, [&] { return axis_fields(&v.x, 4, spec); });
}

bool prop_range(const char* text, glm::vec2& v, const DragSpec& spec,
                const PropOpts<glm::vec2>& opts) {
    return prop_row(text, v, opts, [&] {
        return ImGui::DragFloatRange2("##v", &v.x, &v.y, spec.speed, spec.min, spec.max,
                                      spec.format);
    });
}

bool slider_float(const char* id, float* v, float min, float max, const char* format,
                  ImGuiSliderFlags flags) {
    const bool log = (flags & ImGuiSliderFlags_Logarithmic) != 0;
    return fill_slider(slider_fraction(*v, min, max, log),
                       [&] { return ImGui::SliderFloat(id, v, min, max, format, flags); });
}

bool slider_int(const char* id, int* v, int min, int max, const char* format,
                ImGuiSliderFlags flags) {
    const bool log = (flags & ImGuiSliderFlags_Logarithmic) != 0;
    return fill_slider(slider_fraction(static_cast<float>(*v), static_cast<float>(min),
                                       static_cast<float>(max), log),
                       [&] { return ImGui::SliderInt(id, v, min, max, format, flags); });
}

bool swatch_text_is_dark(Rgba color) {
    const Rgba shown{std::clamp(color.r, 0.0F, 1.0F), std::clamp(color.g, 0.0F, 1.0F),
                     std::clamp(color.b, 0.0F, 1.0F), 1.0F};
    // Black (L = 0) and white (L = 1) give equal contrast at L ~ 0.179.
    return relative_luminance(shown) > 0.1791F;
}

bool color_field(const char* id, float* rgba, bool alpha, bool hdr) {
    ImGui::PushID(id);
    ImGuiColorEditFlags flags = ImGuiColorEditFlags_Float | ImGuiColorEditFlags_AlphaPreviewHalf;
    if (!alpha) flags |= ImGuiColorEditFlags_NoAlpha;
    if (hdr) flags |= ImGuiColorEditFlags_HDR;
    const ImVec4 col(rgba[0], rgba[1], rgba[2], alpha ? rgba[3] : 1.0F);
    const float w = ImGui::CalcItemWidth();
    if (ImGui::ColorButton("##swatch", col, flags, ImVec2(w, ImGui::GetFrameHeight()))) {
        ImGui::OpenPopup("##picker");
    }
    // Values on the swatch, in a contrasting colour.
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    char text[64];
    if (alpha) {
        std::snprintf(text, sizeof(text), "%.2f  %.2f  %.2f  %.2f", static_cast<double>(rgba[0]),
                      static_cast<double>(rgba[1]), static_cast<double>(rgba[2]),
                      static_cast<double>(rgba[3]));
    } else {
        std::snprintf(text, sizeof(text), "%.2f  %.2f  %.2f", static_cast<double>(rgba[0]),
                      static_cast<double>(rgba[1]), static_cast<double>(rgba[2]));
    }
    // The swatch shows the colour as stored (clamped, over the half
    // checkerboard when translucent): black or white text by the swatch's
    // luminance, on a translucent backing of the other so the values stay
    // readable over any colour and the checkerboard.
    const bool dark_text = swatch_text_is_dark({col.x, col.y, col.z, 1.0F});
    constexpr Rgba black{0.0F, 0.0F, 0.0F, 1.0F};
    constexpr Rgba white{1.0F, 1.0F, 1.0F, 1.0F};
    const ImVec2 ts = ImGui::CalcTextSize(text);
    const float pad = scaled(Spacing::xs);
    if (ts.x + pad * 3.0F < max.x - min.x) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 tp(std::round(min.x + pad * 1.5F),
                        std::round(min.y + (max.y - min.y - ts.y) * 0.5F));
        dl->AddRectFilled(ImVec2(tp.x - pad * 0.5F, tp.y - scaled(1.0F)),
                          ImVec2(tp.x + ts.x + pad * 0.5F, tp.y + ts.y + scaled(1.0F)),
                          to_u32(with_alpha(dark_text ? white : black, 0.55F)),
                          scaled(Radius::sm));
        dl->AddText(tp, to_u32(dark_text ? black : white), text);
    }
    item_tooltip("Click to edit");
    bool changed = false;
    if (ImGui::BeginPopup("##picker")) {
        changed = alpha ? ImGui::ColorPicker4("##pick", rgba, flags | ImGuiColorEditFlags_AlphaBar)
                        : ImGui::ColorPicker3("##pick", rgba, flags);
        ImGui::EndPopup();
    }
    ImGui::PopID();
    return changed;
}

bool prop_color3(const char* text, glm::vec3& v, const PropOpts<glm::vec3>& opts, bool hdr) {
    return prop_row(text, v, opts, [&] { return color_field("##v", &v.x, false, hdr); });
}

bool prop_color4(const char* text, glm::vec4& v, const PropOpts<glm::vec4>& opts, bool hdr) {
    return prop_row(text, v, opts, [&] { return color_field("##v", &v.x, true, hdr); });
}

bool prop_text(const char* text, std::string& v, const PropOpts<std::string>& opts,
               bool commit_on_enter) {
    return prop_row(text, v, opts, [&] {
        if (!commit_on_enter) return ImGui::InputText("##v", &v);
        // Edit a copy; apply on Enter or when focus leaves the field.
        std::string draft = v;
        const bool enter = ImGui::InputText("##v", &draft, ImGuiInputTextFlags_EnterReturnsTrue);
        if (enter || (ImGui::IsItemDeactivatedAfterEdit() && draft != v)) {
            v = std::move(draft);
            return true;
        }
        return false;
    });
}

void prop_value(const char* text, std::string_view value, const char* tooltip, bool mono) {
    prop_begin(text, tooltip, false, false);
    ImGui::AlignTextToFramePadding();
    if (mono) push_font(FontRole::mono);
    ImGui::TextUnformatted(value.data(), value.data() + value.size());
    if (mono) pop_font();
    prop_end(false, false);
}

bool prop_enum(const char* text, int& v, std::span<const EnumItem> items,
               const PropOpts<int>& opts) {
    return prop_row(text, v, opts, [&] {
        const char* preview = "?";
        for (const EnumItem& item : items) {
            if (item.value == v) preview = item.label;
        }
        bool changed = false;
        if (ImGui::BeginCombo("##v", preview)) {
            for (const EnumItem& item : items) {
                const bool selected = item.value == v;
                if (ImGui::Selectable(item.label, selected) && !selected) {
                    v = item.value;
                    changed = true;
                }
                if (item.tooltip != nullptr) item_tooltip(item.tooltip);
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        return changed;
    });
}

void prop_note(const char* text) { prop_note(text, tokens().text_secondary); }

void prop_note(const char* text, Rgba color) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(1);
    ImGui::PushTextWrapPos(0.0F);
    text_colored(color, text);
    ImGui::PopTextWrapPos();
}

void prop_separator(const char* title) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    if (title != nullptr) {
        FontScope heading(FontRole::heading);
        text_colored(tokens().text_secondary, title);
    } else {
        ImGui::Dummy(ImVec2(0.0F, scaled(2.0F)));
    }
    // A hairline through the value column's centre continues the heading.
    ImGui::TableSetColumnIndex(1);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float y = std::round(p.y + ImGui::GetFrameHeight() * 0.5F);
    ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, y),
                                        ImVec2(p.x + ImGui::GetContentRegionAvail().x, y),
                                        to_u32(tokens().separator), scaled(1.0F));
    ImGui::Dummy(ImVec2(0.0F, ImGui::GetFrameHeight()));
}

} // namespace engine::editor::ui
