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

// The code part of a line: everything before a `//` comment that is not
// inside a string or character literal ("file://" + p.string() is code).
// Block comments count as code, which errs on the side of reporting.
[[nodiscard]] std::string_view code_of(std::string_view line) {
    char quote = 0; // the open literal's delimiter, 0 outside literals
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (quote != 0) {
            if (c == '\\') {
                ++i; // an escaped character never closes the literal
            } else if (c == quote) {
                quote = 0;
            }
        } else if (c == '"' || c == '\'') {
            quote = c;
        } else if (c == '/' && i + 1 < line.size() && line[i + 1] == '/') {
            return line.substr(0, i);
        }
    }
    return line;
}

// Whether `code` calls string() / generic_string() on an object (`.` or `->`).
[[nodiscard]] bool converts_to_narrow(std::string_view code) {
    for (const std::string_view call : {"string()", "generic_string()"}) {
        for (std::size_t at = code.find(call); at != std::string_view::npos;
             at = code.find(call, at + 1)) {
            const std::string_view before = code.substr(0, at);
            if (before.ends_with('.') || before.ends_with("->")) return true;
        }
    }
    return false;
}

// Lines calling .string() / .generic_string() (as "file:line: text").
[[nodiscard]] std::vector<std::string> narrow_conversions(const fs::path& root,
                                                          const fs::path& file) {
    std::vector<std::string> out;
    std::ifstream in(file, std::ios::binary);
    std::string line;
    for (int n = 1; std::getline(in, line); ++n) {
        if (converts_to_narrow(code_of(line))) {
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

// Source lines holding string literals, as escaped ordinary literals in named
// constants: MSVC's preprocessor mis-parses raw string literals that contain
// quotes when they appear inside macro arguments such as EXPECT_EQ (C2187).
constexpr std::string_view k_url_line = "f(\"file://\" + p.string()); // x";
constexpr std::string_view k_url_code = "f(\"file://\" + p.string()); ";
constexpr std::string_view k_escaped_quote_line = "f(\"a\\\"//\" + p.string());";
constexpr std::string_view k_char_line = "c = '/'; // x";
constexpr std::string_view k_char_code = "c = '/'; ";
constexpr std::string_view k_log_url_line = "log(\"file://\" + p.string());";

TEST(PathEncodingRule, TheScanSeesCodeButNotComments) {
    EXPECT_EQ(code_of("auto s = p.string(); // ok"), "auto s = p.string(); ");
    EXPECT_EQ(code_of("// never with .string()"), "");
    // `//` inside a literal is not a comment.
    EXPECT_EQ(code_of(k_url_line), k_url_code);
    EXPECT_EQ(code_of(k_escaped_quote_line), k_escaped_quote_line);
    EXPECT_EQ(code_of(k_char_line), k_char_code);
}

TEST(PathEncodingRule, CatchesMemberAndPointerCalls) {
    EXPECT_TRUE(converts_to_narrow("auto s = p.string();"));
    EXPECT_TRUE(converts_to_narrow("auto s = p.generic_string();"));
    EXPECT_TRUE(converts_to_narrow("auto s = file.value->string();"));
    EXPECT_TRUE(converts_to_narrow("auto s = it->path().stem()->generic_string();"));
    EXPECT_TRUE(converts_to_narrow(code_of(k_log_url_line)));
    EXPECT_FALSE(converts_to_narrow("std::string s = std::string();"));
    EXPECT_FALSE(converts_to_narrow("const auto s = core::path_to_utf8(p);"));
    EXPECT_FALSE(converts_to_narrow("x.u8string(); y.generic_u8string();"));
}

} // namespace
