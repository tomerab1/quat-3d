#include "engine/core/path_confine.hpp"

#include <string>
#include <system_error>
#include <utility>

#include "engine/core/utf8_path.hpp"

namespace engine::core {

namespace fs = std::filesystem;

std::expected<fs::path, Error> confine_to_root(const fs::path& root, std::string_view relative) {
    const auto fail = [](std::string what) { return std::unexpected(Error{std::move(what)}); };
    if (relative.empty()) return fail("expected a path relative to the project root");
    if (relative.find('\0') != std::string_view::npos) return fail("path contains a NUL byte");
    const fs::path p = path_from_utf8(relative); // file / request JSON is UTF-8
    if (p.is_absolute() || p.has_root_name() || p.has_root_directory()) {
        return fail("absolute paths are not allowed; give a path relative to the project root");
    }
    const fs::path norm = p.lexically_normal();
    if (norm.empty() || norm == ".") return fail("expected a file path, not the project root");
    if (*norm.begin() == "..") return fail("the path escapes the project root");

    // Symlinks inside the project could still point outside: compare the
    // resolved forms of the existing prefixes too.
    std::error_code ec;
    const fs::path canon_root = fs::weakly_canonical(root, ec);
    if (ec) return fail("cannot resolve the project root: " + ec.message());
    const fs::path canon = fs::weakly_canonical(root / norm, ec);
    if (ec) return fail("cannot resolve the path: " + ec.message());
    const fs::path rel = canon.lexically_relative(canon_root);
    if (rel.empty() || *rel.begin() == "..") return fail("the path escapes the project root");
    return root / norm;
}

fs::path relative_inside(const fs::path& root, const fs::path& path) {
    std::error_code ec;
    const fs::path abs_root = fs::absolute(root, ec).lexically_normal();
    if (ec) return {};
    const fs::path abs_path = fs::absolute(path, ec).lexically_normal();
    if (ec) return {};
    fs::path rel = abs_path.lexically_relative(abs_root);
    if (rel.empty() || rel == "." || *rel.begin() == "..") return {};
    return rel;
}

} // namespace engine::core
