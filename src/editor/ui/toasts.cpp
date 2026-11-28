#include "toasts.hpp"

#include <algorithm>
#include <cstdio>
#include <string>

#include <imgui.h>

#include "fonts.hpp"
#include "icons.hpp"
#include "metrics.hpp"
#include "theme.hpp"

namespace engine::editor::ui {

std::uint64_t ToastQueue::push(ToastKind kind, std::string text, float lifetime) {
    if (!items_.empty() && items_.back().kind == kind && items_.back().text == text) {
        items_.back().age = 0.0F;
        return items_.back().id;
    }
    Toast t;
    t.id = next_id_++;
    t.kind = kind;
    t.text = std::move(text);
    t.lifetime = std::max(lifetime, k_fade_s);
    items_.push_back(std::move(t));
    while (items_.size() > k_max) items_.pop_front();
    return items_.back().id;
}

void ToastQueue::update(float dt) {
    const float step = std::max(dt, 0.0F);
    for (Toast& t : items_) t.age += step;
    std::erase_if(items_, [](const Toast& t) { return t.age >= t.lifetime; });
}

void ToastQueue::dismiss(std::uint64_t id) {
    std::erase_if(items_, [id](const Toast& t) { return t.id == id; });
}

float ToastQueue::opacity(const Toast& t) {
    const float left = t.lifetime - t.age;
    return std::clamp(left / k_fade_s, 0.0F, 1.0F);
}

ToastQueue& toasts() {
    static ToastQueue queue;
    return queue;
}

void draw_toasts(float dt, const ImVec2& bottom_right) {
    ToastQueue& queue = toasts();
    queue.update(dt);
    const Theme& t = tokens();
    const float width = scaled(Sizes::toast_width);
    const float pad = scaled(Spacing::sm);
    float y = bottom_right.y - scaled(Spacing::md);
    std::uint64_t dismiss = 0;
    // Newest at the bottom, older ones stacked above.
    for (auto it = queue.items().rbegin(); it != queue.items().rend(); ++it) {
        const Toast& toast = *it;
        const char* glyph = icon::info;
        Rgba accent = t.info;
        switch (toast.kind) {
        case ToastKind::info: break;
        case ToastKind::success: glyph = icon::circle_check; accent = t.success; break;
        case ToastKind::warning: glyph = icon::triangle_alert; accent = t.warning; break;
        case ToastKind::error: glyph = icon::circle_x; accent = t.error; break;
        }
        const float alpha = ToastQueue::opacity(toast);
        const float text_w = width - pad * 3.0F - ImGui::GetFontSize();
        const ImVec2 text_size = ImGui::CalcTextSize(toast.text.c_str(), nullptr, false, text_w);
        const float h = text_size.y + pad * 2.0F;
        const ImVec2 min(bottom_right.x - scaled(Spacing::md) - width, y - h);
        const ImVec2 max(min.x + width, y);
        y = min.y - scaled(Spacing::xs);

        char name[32];
        std::snprintf(name, sizeof(name), "##toast%llu", static_cast<unsigned long long>(toast.id));
        ImGui::SetNextWindowPos(min);
        ImGui::SetNextWindowSize(ImVec2(width, h));
        ImGui::SetNextWindowBgAlpha(0.0F);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0F, 0.0F));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0F);
        const ImGuiWindowFlags flags =
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
            ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoBringToFrontOnFocus;
        if (ImGui::Begin(name, nullptr, flags)) {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const float r = scaled(Radius::lg);
            dl->AddRectFilled(min, max, to_u32(with_alpha(t.bg2, 0.97F * alpha)), r);
            dl->AddRect(min, max, to_u32(with_alpha(t.border, alpha)), r, 0, scaled(1.0F));
            dl->AddRectFilled(min, ImVec2(min.x + scaled(3.0F), max.y),
                              to_u32(with_alpha(accent, alpha)), r, ImDrawFlags_RoundCornersLeft);
            const char* g = ic(glyph);
            float x = min.x + pad;
            if (g[0] != '\0') {
                dl->AddText(ImVec2(x, min.y + pad), to_u32(with_alpha(accent, alpha)), g);
                x += ImGui::GetFontSize() + pad * 0.75F;
            }
            dl->AddText(nullptr, 0.0F, ImVec2(x, min.y + pad), to_u32(with_alpha(t.text, alpha)),
                        toast.text.c_str(), nullptr, text_w);
            ImGui::InvisibleButton("##dismiss", ImVec2(width, h));
            if (ImGui::IsItemClicked()) dismiss = toast.id;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Click to dismiss");
        }
        ImGui::End();
        ImGui::PopStyleVar(2);
    }
    if (dismiss != 0) queue.dismiss(dismiss);
}

} // namespace engine::editor::ui
