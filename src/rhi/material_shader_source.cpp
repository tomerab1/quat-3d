#include "engine/rhi/material_shader_source.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <utility>

namespace engine::rhi {

namespace {

[[nodiscard]] bool ident_start(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_';
}
[[nodiscard]] bool ident_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

// `text` with comments blanked and string literal contents replaced by 'x'
// (quotes kept, so a string can still be recognised). Newlines are kept, so
// offsets map to the same lines.
[[nodiscard]] std::string mask(std::string_view text) {
    std::string out(text);
    std::size_t i = 0;
    while (i < text.size()) {
        if (text.compare(i, 2, "//") == 0) {
            for (; i < text.size() && text[i] != '\n'; ++i) out[i] = ' ';
        } else if (text.compare(i, 2, "/*") == 0) {
            const std::size_t end = text.find("*/", i + 2);
            const std::size_t stop = end == std::string_view::npos ? text.size() : end + 2;
            for (; i < stop; ++i) {
                if (text[i] != '\n') out[i] = ' ';
            }
        } else if (text[i] == '"') {
            ++i; // opening quote kept
            while (i < text.size() && text[i] != '"' && text[i] != '\n') {
                // An escape covers the next character, but never a newline.
                if (text[i] == '\\' && i + 1 < text.size() && text[i + 1] != '\n') {
                    out[i] = 'x';
                    ++i;
                }
                out[i] = 'x';
                ++i;
            }
            if (i < text.size() && text[i] == '"') ++i; // closing quote kept
        } else {
            ++i;
        }
    }
    return out;
}

[[nodiscard]] std::size_t line_of(std::string_view text, std::size_t offset) {
    return 1 + static_cast<std::size_t>(
                   std::count(text.begin(), text.begin() + static_cast<std::ptrdiff_t>(offset),
                              '\n'));
}

[[nodiscard]] std::unexpected<core::Error> at(std::string_view text, std::size_t offset,
                                              const std::string& what) {
    return std::unexpected(
        core::Error{"source line " + std::to_string(line_of(text, offset)) + ": " + what});
}

[[nodiscard]] std::size_t skip_space(std::string_view s, std::size_t i) {
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])) != 0) ++i;
    return i;
}

[[nodiscard]] std::string module_list(std::span<const std::string> modules) {
    std::string out;
    for (const std::string& m : modules) out += (out.empty() ? "" : ", ") + m;
    return out;
}

} // namespace

std::expected<void, core::Error> check_material_source(std::string_view text,
                                                       std::span<const std::string> modules) {
    const std::string code = mask(text);
    for (std::size_t i = 0; i < code.size();) {
        const char c = code[i];
        if (c == '#') {
            return at(text, i,
                      "preprocessor directives (#include, #define, ...) are not allowed in "
                      "material shaders");
        }
        if (c == '\\') {
            return at(text, i, "a backslash is only allowed inside a string literal");
        }
        if (c == '"') { // a masked string literal: skip it
            ++i;
            while (i < code.size() && code[i] != '"' && code[i] != '\n') ++i;
            if (i < code.size() && code[i] == '"') ++i;
            continue;
        }
        if (!ident_start(c) || (i > 0 && ident_char(code[i - 1]))) {
            ++i;
            continue;
        }
        std::size_t end = i;
        while (end < code.size() && ident_char(code[end])) ++end;
        const std::string_view word(code.data() + i, end - i);
        if (word == "__include") {
            return at(text, i, "`__include` is not allowed in material shaders");
        }
        if (word == "import") {
            std::size_t j = skip_space(code, end);
            if (j < code.size() && code[j] == '"') {
                return at(text, j,
                          "`import \"<file>\"` is not allowed; import a shader library module by "
                          "name, e.g. `import material;`");
            }
            std::size_t name_end = j;
            while (name_end < code.size() && (ident_char(code[name_end]) || code[name_end] == '.')) {
                ++name_end;
            }
            const std::string name(code.data() + j, name_end - j);
            if (name.empty() || name.find('.') != std::string::npos || !ident_start(name[0]) ||
                std::find(modules.begin(), modules.end(), name) == modules.end()) {
                return at(text, j,
                          "`import " + name + ";` does not name a shader library module (" +
                              module_list(modules) + ")");
            }
            i = name_end;
            continue;
        }
        i = end;
    }
    return {};
}

} // namespace engine::rhi
