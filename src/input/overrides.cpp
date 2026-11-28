// Rebinding and its per-user persistence, and action-map files — see
// include/engine/input/actions.hpp.

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>

#include "engine/core/json_fields.hpp"
#include "engine/core/utf8_path.hpp"
#include "engine/input/actions.hpp"

namespace engine::input {

namespace {

using Json = nlohmann::json;

constexpr std::uintmax_t k_max_file_bytes = 1U << 20;

[[nodiscard]] std::expected<Json, core::Error> read_json_file(const std::filesystem::path& path,
                                                              bool missing_ok, bool& missing) {
    missing = false;
    std::error_code ec;
    const bool exists = std::filesystem::exists(path, ec);
    if (!exists) {
        missing = true;
        if (missing_ok) return Json::object();
        return std::unexpected(core::Error{core::path_to_utf8(path) + ": no such file"});
    }
    const std::uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec || size > k_max_file_bytes) {
        return std::unexpected(core::Error{core::path_to_utf8(path) + ": unreadable or larger than 1 MiB"});
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::unexpected(core::Error{core::path_to_utf8(path) + ": cannot open"});
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    Json json = Json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (json.is_discarded()) {
        return std::unexpected(core::Error{core::path_to_utf8(path) + ": not valid JSON"});
    }
    return json;
}

} // namespace

std::expected<void, core::Error> Actions::rebind(std::string_view name, std::vector<Binding> bindings) {
    const int i = find(name);
    if (i < 0) return std::unexpected(core::Error{"unknown action '" + std::string(name) + "'"});
    if (bindings.size() > k_max_bindings) {
        return std::unexpected(core::Error{"bindings: at most " + std::to_string(k_max_bindings)});
    }
    const auto at = static_cast<std::size_t>(i);
    map_.actions[at].bindings = std::move(bindings);
    runtime_[at] = Runtime{};
    return {};
}

std::expected<void, core::Error> Actions::reset_bindings(std::string_view name) {
    const int i = find(name);
    const int d = defaults_.find_action(name);
    if (i < 0 || d < 0) return std::unexpected(core::Error{"unknown action '" + std::string(name) + "'"});
    map_.actions[static_cast<std::size_t>(i)].bindings = defaults_.actions[static_cast<std::size_t>(d)].bindings;
    runtime_[static_cast<std::size_t>(i)] = Runtime{};
    return {};
}

void Actions::reset_all_bindings() {
    for (std::size_t i = 0; i < map_.actions.size(); ++i) map_.actions[i].bindings = defaults_.actions[i].bindings;
    reset_runtime();
}

Json Actions::overrides_json() const {
    Json bindings = Json::object();
    for (std::size_t i = 0; i < map_.actions.size(); ++i) {
        if (map_.actions[i].bindings == defaults_.actions[i].bindings) continue;
        Json list = Json::array();
        for (const Binding& b : map_.actions[i].bindings) list.push_back(binding_json(b));
        bindings[map_.actions[i].name] = std::move(list);
    }
    return Json{{"version", 1}, {"bindings", std::move(bindings)}};
}

std::expected<void, core::Error> Actions::apply_overrides(const Json& json) {
    core::JsonFields f(json, "overrides");
    int version = 1;
    f.integer("version", version, 1, 1);
    const Json* bindings = f.raw("bindings");
    if (bindings != nullptr && !bindings->is_object()) f.fail("bindings", "expected an object of action -> bindings");
    if (auto ok = f.finish(); !ok) return std::unexpected(ok.error());
    if (bindings == nullptr) return {};

    // Validate everything first: a bad file changes nothing.
    std::vector<std::pair<int, std::vector<Binding>>> parsed;
    for (const auto& [name, list] : bindings->items()) {
        const std::string at = "overrides.bindings." + name;
        const int i = find(name);
        if (i < 0) continue; // an action this map no longer has: ignored, kept out of the next save
        if (!list.is_array() || list.size() > k_max_bindings) {
            return std::unexpected(core::Error{at + ": expected an array of at most " + std::to_string(k_max_bindings) + " bindings"});
        }
        std::vector<Binding> out;
        for (std::size_t k = 0; k < list.size(); ++k) {
            auto b = parse_binding(list[k], at + "[" + std::to_string(k) + "]");
            if (!b) return std::unexpected(b.error());
            out.push_back(std::move(*b));
        }
        parsed.emplace_back(i, std::move(out));
    }
    for (auto& [i, list] : parsed) {
        map_.actions[static_cast<std::size_t>(i)].bindings = std::move(list);
        runtime_[static_cast<std::size_t>(i)] = Runtime{};
    }
    return {};
}

std::expected<void, core::Error> load_overrides_file(Actions& actions, const std::filesystem::path& path) {
    bool missing = false;
    auto json = read_json_file(path, /*missing_ok=*/true, missing);
    if (!json) return std::unexpected(json.error());
    if (missing) return {};
    if (auto ok = actions.apply_overrides(*json); !ok) {
        return std::unexpected(core::Error{core::path_to_utf8(path) + ": " + ok.error().message});
    }
    return {};
}

std::expected<void, core::Error> save_overrides_file(const Actions& actions, const std::filesystem::path& path) {
    std::error_code ec;
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
    std::filesystem::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return std::unexpected(core::Error{core::path_to_utf8(tmp) + ": cannot write"});
        out << actions.overrides_json().dump(2, ' ', false, Json::error_handler_t::replace) << '\n';
        if (!out) return std::unexpected(core::Error{core::path_to_utf8(tmp) + ": write failed"});
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        return std::unexpected(core::Error{core::path_to_utf8(path) + ": cannot replace the file"});
    }
    return {};
}

std::expected<ActionMap, core::Error> load_action_map_file(const std::filesystem::path& path) {
    bool missing = false;
    auto json = read_json_file(path, /*missing_ok=*/false, missing);
    if (!json) return std::unexpected(json.error());
    auto map = parse_action_map(*json);
    if (!map) return std::unexpected(core::Error{core::path_to_utf8(path) + ": " + map.error().message});
    return map;
}

} // namespace engine::input
