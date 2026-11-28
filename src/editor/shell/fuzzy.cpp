#include "fuzzy.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>
#include <utility>

namespace engine::editor::shell {

namespace {

[[nodiscard]] char lower(char c) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

[[nodiscard]] bool is_word_start(std::string_view text, std::size_t i) {
    if (i == 0) return true;
    const unsigned char prev = static_cast<unsigned char>(text[i - 1]);
    const unsigned char cur = static_cast<unsigned char>(text[i]);
    if (std::isalnum(prev) == 0) return true;                   // after space, '.', '/', ...
    return std::islower(prev) != 0 && std::isupper(cur) != 0;   // camelCase hump
}

constexpr int k_match = 1;
constexpr int k_word_start = 8;
constexpr int k_consecutive = 5;
constexpr int k_first_char = 6;     // query starts where the text starts
constexpr int k_gap_penalty = 1;    // per skipped character between matches
constexpr int k_lead_penalty = 1;   // per character before the first match (capped)
constexpr int k_max_lead_penalty = 6;
constexpr std::size_t k_max_len = 256; // longer texts / queries are not scored (DP size)

} // namespace

std::optional<int> fuzzy_score(std::string_view query, std::string_view text) {
    if (query.empty()) return 0;
    if (query.size() > text.size() || text.size() > k_max_len) return std::nullopt;

    // best[j] = best score with query[0..i] matched and query[i] at text[j];
    // a small DP picks the best alignment (greedy matching would miss
    // "save scene" for "ss" because the first 's' grabs "save" only).
    constexpr int none = std::numeric_limits<int>::min() / 2;
    std::array<int, k_max_len> prev{};
    std::array<int, k_max_len> cur{};
    const std::size_t n = text.size();
    for (std::size_t j = 0; j < n; ++j) {
        if (lower(text[j]) != lower(query[0])) {
            prev[j] = none;
            continue;
        }
        int s = k_match;
        if (is_word_start(text, j)) s += k_word_start;
        if (j == 0) s += k_first_char;
        s -= std::min<int>(static_cast<int>(j) * k_lead_penalty, k_max_lead_penalty);
        prev[j] = s;
    }
    for (std::size_t i = 1; i < query.size(); ++i) {
        // Running best of prev[k] + k (gap penalty is linear in the distance).
        int best_before = none;
        for (std::size_t j = 0; j < n; ++j) {
            cur[j] = none;
            if (j > 0 && prev[j - 1] != none) {
                best_before = std::max(best_before,
                                       prev[j - 1] + static_cast<int>(j - 1) * k_gap_penalty);
            }
            if (lower(text[j]) != lower(query[i]) || j < i) continue;
            int s = none;
            if (j > 0 && prev[j - 1] != none) s = prev[j - 1] + k_consecutive; // adjacent
            if (best_before != none) {
                const int gap = best_before - static_cast<int>(j - 1) * k_gap_penalty -
                                k_gap_penalty;
                s = std::max(s, gap);
            }
            if (s == none) continue;
            s += k_match;
            if (is_word_start(text, j)) s += k_word_start;
            cur[j] = s;
        }
        std::swap(prev, cur);
    }
    int best = none;
    for (std::size_t j = 0; j < n; ++j) best = std::max(best, prev[j]);
    if (best == none) return std::nullopt;
    return best;
}

std::vector<std::size_t> fuzzy_rank(std::string_view query,
                                    std::span<const std::string_view> texts) {
    std::vector<std::pair<int, std::size_t>> scored;
    for (std::size_t i = 0; i < texts.size(); ++i) {
        if (const auto s = fuzzy_score(query, texts[i])) scored.emplace_back(*s, i);
    }
    std::stable_sort(scored.begin(), scored.end(),
                     [](const auto& a, const auto& b) { return a.first > b.first; });
    std::vector<std::size_t> out;
    out.reserve(scored.size());
    for (const auto& [score, index] : scored) out.push_back(index);
    return out;
}

} // namespace engine::editor::shell
