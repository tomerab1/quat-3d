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
        const char* glyph = ic(icon::rotate_ccw);
        const float side = reset_width();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, to_imvec4(tokens().bg3));
        ImGui::PushStyleColor(ImGuiCol_Text, to_imvec4(tokens().accent_text));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0F);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0F, 0.0F));
        clicked = ImGui::Button(glyph[0] != '\0' ? glyph : "R", ImVec2(side, side));
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(3);
        item_tooltip("Reset to default");
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
            return ImGui::SliderFloat("##v", &v, spec.min, spec.max, spec.format);
        }
        return ImGui::DragFloat("##v", &v, spec.speed, spec.min, spec.max, spec.format);
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
            return ImGui::SliderInt("##v", &v, spec.min, spec.max, spec.format);
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

bool prop_color3(const char* text, glm::vec3& v, const PropOpts<glm::vec3>& opts, bool hdr) {
    return prop_row(text, v, opts, [&] {
        ImGuiColorEditFlags flags = ImGuiColorEditFlags_Float;
        if (hdr) flags |= ImGuiColorEditFlags_HDR;
        return ImGui::ColorEdit3("##v", &v.x, flags);
    });
}

bool prop_color4(const char* text, glm::vec4& v, const PropOpts<glm::vec4>& opts, bool hdr) {
    return prop_row(text, v, opts, [&] {
        ImGuiColorEditFlags flags = ImGuiColorEditFlags_Float |
                                    ImGuiColorEditFlags_AlphaPreviewHalf |
                                    ImGuiColorEditFlags_AlphaBar;
        if (hdr) flags |= ImGuiColorEditFlags_HDR;
        return ImGui::ColorEdit4("##v", &v.x, flags);
    });
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
