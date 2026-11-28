// game.json format 2's "engine" version range — see
// include/engine/project/game_project.hpp. A range is comma-separated
// comparators, each an operator (>=, >, <=, <, =; none means =) and a
// MAJOR.MINOR[.PATCH] version, all of which must hold: ">=0.4,<0.5". A
// version without PATCH stands for PATCH 0. The syntax is the comparator-set
// subset of the npm / Cargo semver range grammars (no ^, ~, wildcards or ||).

#include <array>
#include <charconv>
#include <string>

#include "engine/project/game_project.hpp"

namespace engine::project {

namespace {

struct Comparator {
    std::string_view op;
    std::array<int, 3> v{};
};

[[nodiscard]] std::expected<Comparator, core::Error> parse_comparator(std::string_view text) {
    const auto bad = [&] {
        return std::unexpected(core::Error{"engine: '" + std::string(text) +
                                           "' is not a comparator like \">=0.4\" or \"<0.5.2\""});
    };
    while (!text.empty() && text.front() == ' ') text.remove_prefix(1);
    while (!text.empty() && text.back() == ' ') text.remove_suffix(1);
    Comparator c;
    for (const std::string_view op : {">=", "<=", ">", "<", "="}) {
        if (text.starts_with(op)) {
            c.op = op;
            text.remove_prefix(op.size());
            break;
        }
    }
    if (c.op.empty()) c.op = "=";
    int parts = 0;
    while (true) {
        int value = 0;
        const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (ec != std::errc{} || end == text.data() || value < 0 || parts == 3) return bad();
        c.v[static_cast<std::size_t>(parts++)] = value;
        text.remove_prefix(static_cast<std::size_t>(end - text.data()));
        if (text.empty()) break;
        if (text.front() != '.') return bad();
        text.remove_prefix(1);
    }
    if (parts < 2) return bad();
    return c;
}

[[nodiscard]] bool holds(const Comparator& c, const std::array<int, 3>& v) {
    if (c.op == ">=") return v >= c.v;
    if (c.op == ">") return v > c.v;
    if (c.op == "<=") return v <= c.v;
    if (c.op == "<") return v < c.v;
    return v == c.v;
}

template <typename Fn>
[[nodiscard]] std::expected<void, core::Error> each_comparator(std::string_view range, Fn&& fn) {
    if (range.empty() || range.size() > 128) {
        return std::unexpected(core::Error{"engine: expected a version range like \">=0.4,<0.5\""});
    }
    std::size_t start = 0;
    while (start <= range.size()) {
        const std::size_t comma = range.find(',', start);
        const std::string_view part =
            range.substr(start, comma == std::string_view::npos ? std::string_view::npos : comma - start);
        auto c = parse_comparator(part);
        if (!c) return std::unexpected(c.error());
        if (auto ok = fn(*c); !ok) return ok;
        if (comma == std::string_view::npos) break;
        start = comma + 1;
    }
    return {};
}

} // namespace

std::expected<void, core::Error> validate_engine_range(std::string_view range) {
    return each_comparator(range, [](const Comparator&) -> std::expected<void, core::Error> { return {}; });
}

std::expected<void, core::Error> check_engine_range(std::string_view range, int major, int minor,
                                                    int patch) {
    const std::array<int, 3> v{major, minor, patch};
    return each_comparator(range, [&](const Comparator& c) -> std::expected<void, core::Error> {
        if (holds(c, v)) return {};
        return std::unexpected(core::Error{
            "engine: this game needs engine " + std::string(range) + "; this is quat-3d " +
            std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch)});
    });
}

} // namespace engine::project
