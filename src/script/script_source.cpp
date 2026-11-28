// Script files — see include/engine/script/script_source.hpp.

#include "engine/script/script_source.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <system_error>

#include "engine/scene/script_component.hpp"
#include "sol_config.hpp"

namespace engine::script {

namespace {

namespace fs = std::filesystem;

// Parses the line number out of "<chunk>:<line>: message".
[[nodiscard]] int error_line(std::string_view message, std::string_view chunk_name) {
    if (!message.starts_with(chunk_name)) return 0;
    std::size_t i = chunk_name.size();
    if (i >= message.size() || message[i] != ':') return 0;
    int line = 0;
    for (++i; i < message.size() && message[i] >= '0' && message[i] <= '9'; ++i) {
        line = line * 10 + (message[i] - '0');
        if (line > 100'000'000) return 0;
    }
    return line;
}

// A bare state for compiling only: nothing is opened, nothing runs.
struct StateDeleter {
    void operator()(lua_State* L) const { lua_close(L); }
};

} // namespace

std::expected<void, SyntaxError> check_syntax(std::string_view source, std::string_view chunk_name) {
    if (source.size() > k_max_script_bytes) {
        return std::unexpected(SyntaxError{0, "script larger than " +
                                                  std::to_string(k_max_script_bytes) + " bytes"});
    }
    const std::unique_ptr<lua_State, StateDeleter> L(luaL_newstate());
    if (!L) return std::unexpected(SyntaxError{0, "cannot create a Lua state"});
    const std::string name = "@" + std::string(chunk_name);
    const int status =
        luaL_loadbufferx(L.get(), source.data(), source.size(), name.c_str(), "t");
    if (status == LUA_OK) return {};
    std::size_t len = 0;
    const char* msg = lua_tolstring(L.get(), -1, &len);
    std::string text = msg != nullptr ? std::string(msg, len) : std::string("unknown error");
    const int line = error_line(text, chunk_name);
    return std::unexpected(SyntaxError{line, std::move(text)});
}

std::expected<fs::path, core::Error> script_file(const fs::path& scripts_root,
                                                 std::string_view relative) {
    if (auto ok = scene::validate_script_path(relative); !ok) return std::unexpected(ok.error());
    const fs::path file = scripts_root / fs::path(std::string(relative));
    std::error_code ec;
    const fs::path canon_root = fs::weakly_canonical(scripts_root, ec);
    if (ec) return std::unexpected(core::Error{"cannot resolve the scripts directory: " + ec.message()});
    const fs::path canon = fs::weakly_canonical(file, ec);
    if (ec) return std::unexpected(core::Error{"cannot resolve the script path: " + ec.message()});
    const fs::path rel = canon.lexically_relative(canon_root);
    if (rel.empty() || *rel.begin() == "..") {
        return std::unexpected(core::Error{"the script path resolves outside assets/scripts/"});
    }
    return file;
}

std::expected<std::string, core::Error> read_script(const fs::path& file) {
    std::error_code ec;
    const auto size = fs::file_size(file, ec);
    if (ec) return std::unexpected(core::Error{"cannot read " + file.filename().string() + ": " + ec.message()});
    if (size > k_max_script_bytes) {
        return std::unexpected(core::Error{file.filename().string() + " is larger than " +
                                           std::to_string(k_max_script_bytes) + " bytes"});
    }
    std::ifstream in(file, std::ios::binary);
    if (!in) return std::unexpected(core::Error{"cannot open " + file.filename().string()});
    std::string text(static_cast<std::size_t>(size), '\0');
    in.read(text.data(), static_cast<std::streamsize>(text.size()));
    text.resize(static_cast<std::size_t>(in.gcount()));
    return text;
}

std::vector<std::string> list_scripts(const fs::path& scripts_root) {
    std::vector<std::string> out;
    std::error_code ec;
    fs::recursive_directory_iterator it(scripts_root, fs::directory_options::skip_permission_denied, ec);
    for (; !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (out.size() >= k_max_listed_scripts) break;
        if (!it->is_regular_file(ec) || it->path().extension() != ".lua") continue;
        std::string rel = it->path().lexically_relative(scripts_root).generic_string();
        if (scene::validate_script_path(rel)) out.push_back(std::move(rel));
    }
    std::ranges::sort(out);
    return out;
}

} // namespace engine::script
