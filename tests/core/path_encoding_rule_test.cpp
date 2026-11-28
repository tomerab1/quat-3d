// Paths leave std::filesystem as UTF-8 (engine/core/utf8_path.hpp), never
// through path::string() / generic_string().
//
// On Windows those convert to the ANSI code page and throw std::system_error
// for any character the code page lacks; the engine builds without
// exceptions, so the process dies (MSVC fast-fails with 0xC0000409, which Git
// Bash reports as exit 127). The packaged editor did exactly that when
// unpacked under "smoke dir ü 画": loading its UI fonts passed
// <data root>/assets/fonts/... to ImGui through .string(). Linux and macOS
// never fail this way, so the rule is checked on the sources, on every
// platform.

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "engine/core/utf8_path.hpp"

namespace {

namespace fs = std::filesystem;

[[nodiscard]] bool is_cpp_source(const fs::path& p) {
    const fs::path ext = p.extension();
    return ext == ".cpp" || ext == ".hpp" || ext == ".h" || ext == ".inl";
}

// The code part of a line: everything before a `//` comment.
[[nodiscard]] std::string_view code_of(std::string_view line) {
    const std::size_t comment = line.find("//");
    return comment == std::string_view::npos ? line : line.substr(0, comment);
}

// Lines calling .string() / .generic_string() (as "file:line: text").
[[nodiscard]] std::vector<std::string> narrow_conversions(const fs::path& root,
                                                          const fs::path& file) {
    std::vector<std::string> out;
    std::ifstream in(file, std::ios::binary);
    std::string line;
    for (int n = 1; std::getline(in, line); ++n) {
        const std::string_view code = code_of(line);
        if (code.find(".string()") != std::string_view::npos ||
            code.find(".generic_string()") != std::string_view::npos) {
            out.push_back(engine::core::path_to_generic_utf8(file.lexically_relative(root)) + ":" +
                          std::to_string(n) + ": " + line);
        }
    }
    return out;
}

TEST(PathEncodingRule, SourcesNeverConvertPathsToTheNarrowEncoding) {
    const fs::path root = engine::core::path_from_utf8(QUAT_PROJECT_ROOT);
    std::vector<std::string> found;
    std::size_t scanned = 0;
    // The C++ trees (not tools/quat-mcp, whose target/ may hold build output).
    for (const char* dir : {"src", "include", "tools/asset-gen", "tools/quat-cook",
                            "tools/quat-pack"}) {
        std::error_code ec;
        for (fs::recursive_directory_iterator it(root / dir, ec), end; !ec && it != end;
             it.increment(ec)) {
            if (!it->is_regular_file(ec) || !is_cpp_source(it->path())) continue;
            ++scanned;
            for (std::string& hit : narrow_conversions(root, it->path())) {
                found.push_back(std::move(hit));
            }
        }
    }
    EXPECT_GT(scanned, 100U) << "the source tree was not found under " << QUAT_PROJECT_ROOT;
    std::string report;
    for (const std::string& f : found) report += "\n  " + f;
    EXPECT_TRUE(found.empty())
        << "use engine::core::path_to_utf8 / path_to_generic_utf8 instead of .string() / "
           ".generic_string() (they abort on Windows for non-ASCII paths):"
        << report;
}

TEST(PathEncodingRule, TheScanSeesCodeButNotComments) {
    EXPECT_EQ(code_of("auto s = p.string(); // ok"), "auto s = p.string(); ");
    EXPECT_EQ(code_of("// never with .string()"), "");
}

} // namespace
