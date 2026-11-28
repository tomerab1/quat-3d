#pragma once

// Fuzzy matching for the command palette (ROADMAP E8.2): the query's
// characters must appear in order in the text (case-insensitive); the score
// rewards matches at word starts, consecutive runs and an early first match,
// so "ss" ranks "Save Scene" above "Show Stats ..." above "Toggle Snapping".

#include <cstddef>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace engine::editor::shell {

// Higher is better; nullopt when `query` is not a subsequence of `text`. An
// empty query matches everything with score 0.
[[nodiscard]] std::optional<int> fuzzy_score(std::string_view query, std::string_view text);

// Indices of `texts` matching `query`, best first (ties keep input order).
[[nodiscard]] std::vector<std::size_t> fuzzy_rank(std::string_view query,
                                                  std::span<const std::string_view> texts);

} // namespace engine::editor::shell
