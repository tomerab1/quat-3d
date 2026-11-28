// Toast queue (ROADMAP E8.1): auto-dismiss, fade, de-duplication, cap.

#ifdef ENGINE_EDITOR

#include <gtest/gtest.h>

#include "editor/ui/toasts.hpp"

namespace {

namespace ui = engine::editor::ui;

TEST(EditorUiToasts, ExpireAfterTheirLifetime) {
    ui::ToastQueue q;
    q.push(ui::ToastKind::success, "Saved", 2.0F);
    q.update(1.0F);
    ASSERT_EQ(q.items().size(), 1U);
    q.update(1.01F);
    EXPECT_TRUE(q.items().empty());
}

TEST(EditorUiToasts, FadeOutAtTheEnd) {
    ui::ToastQueue q;
    q.push(ui::ToastKind::info, "x", 2.0F);
    EXPECT_FLOAT_EQ(ui::ToastQueue::opacity(q.items().front()), 1.0F);
    q.update(2.0F - ui::ToastQueue::k_fade_s * 0.5F);
    EXPECT_NEAR(ui::ToastQueue::opacity(q.items().front()), 0.5F, 1e-4F);
}

TEST(EditorUiToasts, RepeatRestartsInsteadOfStacking) {
    ui::ToastQueue q;
    const auto a = q.push(ui::ToastKind::error, "Load failed", 3.0F);
    q.update(2.0F);
    const auto b = q.push(ui::ToastKind::error, "Load failed", 3.0F);
    EXPECT_EQ(a, b);
    ASSERT_EQ(q.items().size(), 1U);
    EXPECT_FLOAT_EQ(q.items().front().age, 0.0F);
    q.push(ui::ToastKind::warning, "Load failed", 3.0F); // different kind: new toast
    EXPECT_EQ(q.items().size(), 2U);
}

TEST(EditorUiToasts, CappedOldestFirstAndDismissable) {
    ui::ToastQueue q;
    for (int i = 0; i < 8; ++i) q.push(ui::ToastKind::info, "t" + std::to_string(i));
    ASSERT_EQ(q.items().size(), ui::ToastQueue::k_max);
    EXPECT_EQ(q.items().front().text, "t3");
    const auto id = q.items().back().id;
    q.dismiss(id);
    EXPECT_EQ(q.items().size(), ui::ToastQueue::k_max - 1);
    EXPECT_EQ(q.items().back().text, "t6");
    q.update(-5.0F); // negative dt never revives or ages backwards
    EXPECT_FLOAT_EQ(q.items().front().age, 0.0F);
}

} // namespace

#endif // ENGINE_EDITOR
