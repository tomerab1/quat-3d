#include "engine/rhi/material_shader_source.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

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

// ---- GPU safety (issue #110) -------------------------------------------------
// A material runs for every pixel of the GBuffer pass; one that reads memory
// it does not own or never terminates takes the GPU down (device lost). So a
// model-authored module may not use inline SPIR-V, pointers or raw
// buffer-device-address access, nor loops that obviously never end.

constexpr std::string_view k_bounded_loops =
    "loops need a bound known at compile time (an endless loop hangs the GPU and the device "
    "is lost)";

// Identifiers that are refused wherever they appear, and why.
struct ForbiddenWord {
    std::string_view word;
    std::string_view why;
};
constexpr std::array k_forbidden_words{
    ForbiddenWord{"spirv_asm", "inline SPIR-V (`spirv_asm`) is not allowed in material shaders"},
    ForbiddenWord{"Ptr", "pointers (`Ptr<T>`) are not allowed in material shaders"},
    ForbiddenWord{"ImmutablePtr", "pointers (`ImmutablePtr<T>`) are not allowed in material "
                                  "shaders"},
    ForbiddenWord{"NativePtr", "pointers (`NativePtr<T>`) are not allowed in material shaders"},
    ForbiddenWord{"ConstBufferPointer", "buffer device addresses (`ConstBufferPointer<T>`) are "
                                        "not allowed in material shaders"},
    ForbiddenWord{"RawBufferLoad", "raw buffer-device-address loads (`RawBufferLoad`) are not "
                                   "allowed in material shaders"},
    ForbiddenWord{"RawBufferStore", "raw buffer-device-address stores (`RawBufferStore`) are "
                                    "not allowed in material shaders"},
    ForbiddenWord{"buffer_reference", "buffer device addresses (`buffer_reference`) are not "
                                      "allowed in material shaders"},
    ForbiddenWord{"vk", "`vk::` attributes and intrinsics (raw buffer access, inline SPIR-V "
                        "instructions) are not allowed in material shaders"},
};

// A built-in scalar / vector / matrix type name (`float`, `uint3`,
// `float4x4`, `int64_t`, ...) or `void`: a `*` after one declares a pointer.
[[nodiscard]] bool builtin_type_name(std::string_view word) {
    constexpr std::array<std::string_view, 18> k_scalars{
        "float",    "half",    "double",   "int",      "uint",      "bool",
        "void",     "int8_t",  "uint8_t",  "int16_t",  "uint16_t",  "int32_t",
        "uint32_t", "int64_t", "uint64_t", "float16_t", "float32_t", "float64_t"};
    const auto dim = [](char c) { return c >= '1' && c <= '4'; };
    for (const std::string_view base : k_scalars) {
        if (!word.starts_with(base)) continue;
        const std::string_view rest = word.substr(base.size());
        if (rest.empty() || (rest.size() == 1 && dim(rest[0])) ||
            (rest.size() == 3 && dim(rest[0]) && rest[1] == 'x' && dim(rest[2]))) {
            return true;
        }
    }
    return false;
}

// The last non-space character before `i` (0 at the start).
[[nodiscard]] char prev_char(std::string_view s, std::size_t i) {
    while (i > 0) {
        --i;
        if (std::isspace(static_cast<unsigned char>(s[i])) == 0) return s[i];
    }
    return '\0';
}

// The identifier ending right before `i` (spaces skipped), or "".
[[nodiscard]] std::string_view prev_word(std::string_view s, std::size_t i) {
    while (i > 0 && std::isspace(static_cast<unsigned char>(s[i - 1])) != 0) --i;
    std::size_t start = i;
    while (start > 0 && ident_char(s[start - 1])) --start;
    if (start == i || !ident_start(s[start])) return {};
    return s.substr(start, i - start);
}

// Whether an operator at `i` stands where an operand starts (a unary
// operator): after an opening bracket, a separator, another operator or
// `return`, rather than after a value.
[[nodiscard]] bool operand_position(std::string_view s, std::size_t i) {
    const char p = prev_char(s, i);
    if (p == '\0') return true;
    if (prev_word(s, i) == "return") return true;
    if (p == '+' || p == '-') {
        // `i++ * 2`: a postfix increment ends an operand.
        std::size_t j = i;
        while (j > 0 && std::isspace(static_cast<unsigned char>(s[j - 1])) != 0) --j;
        if (j >= 2 && s[j - 2] == p) return false;
    }
    return std::string_view("(,={};[?:+-/%&|^!~<").find(p) != std::string_view::npos;
}

// Type names the module declares (`struct M`, `typealias T`, ...): a `*`
// after one declares a pointer too.
[[nodiscard]] std::vector<std::string_view> declared_types(std::string_view code) {
    std::vector<std::string_view> out;
    for (std::size_t i = 0; i < code.size();) {
        if (!ident_start(code[i]) || (i > 0 && ident_char(code[i - 1]))) {
            ++i;
            continue;
        }
        std::size_t end = i;
        while (end < code.size() && ident_char(code[end])) ++end;
        const std::string_view word = code.substr(i, end - i);
        if (word == "struct" || word == "class" || word == "enum" || word == "interface" ||
            word == "typealias" || word == "typedef") {
            const std::size_t j = skip_space(code, end);
            std::size_t name_end = j;
            while (name_end < code.size() && ident_char(code[name_end])) ++name_end;
            if (name_end > j && ident_start(code[j])) out.push_back(code.substr(j, name_end - j));
        }
        i = end;
    }
    return out;
}

// A `*` at `i` that makes or uses a pointer: a pointer type (`float*`,
// `M*`, `vector<float, 3>*`, `(T*)x`, `bit_cast<T*>`) or a dereference.
// Multiplication (`a * b`, `x *= 2`) passes.
[[nodiscard]] std::optional<std::string> pointer_star(std::string_view code, std::size_t i,
                                                      std::span<const std::string_view> types) {
    if (i + 1 < code.size() && code[i + 1] == '=') return std::nullopt; // `*=`
    const std::size_t next = skip_space(code, i + 1);
    if (next < code.size() && std::string_view(")>,*;]").find(code[next]) != std::string_view::npos) {
        return "pointer types (`T*`) are not allowed in material shaders";
    }
    const std::string_view before = prev_word(code, i);
    if (!before.empty() && (builtin_type_name(before) ||
                            std::find(types.begin(), types.end(), before) != types.end())) {
        return "pointer types (`" + std::string(before) +
               "*`) are not allowed in material shaders";
    }
    if (prev_char(code, i) == '>') {
        return "pointer types (`T*`) are not allowed in material shaders";
    }
    if (operand_position(code, i)) {
        return std::string("pointer dereference (`*p`) is not allowed in material shaders");
    }
    return std::nullopt;
}

// `for (init; ; step)`: no condition, so the loop never ends by itself.
[[nodiscard]] bool for_without_condition(std::string_view code, std::size_t after_for) {
    std::size_t j = skip_space(code, after_for);
    if (j >= code.size() || code[j] != '(') return false;
    int depth = 0;
    for (; j < code.size(); ++j) {
        if (code[j] == '(') ++depth;
        if (code[j] == ')' && --depth == 0) return false;
        if (code[j] == ';' && depth == 1) break;
    }
    const std::size_t k = skip_space(code, j + 1);
    return j < code.size() && k < code.size() && code[k] == ';';
}

// `while (true)` / `while (1)`.
[[nodiscard]] bool while_constant_true(std::string_view code, std::size_t after_while) {
    std::size_t j = skip_space(code, after_while);
    if (j >= code.size() || code[j] != '(') return false;
    j = skip_space(code, j + 1);
    std::size_t end = j;
    while (end < code.size() && ident_char(code[end])) ++end;
    const std::string_view cond = code.substr(j, end - j);
    return (cond == "true" || cond == "1") && skip_space(code, end) < code.size() &&
           code[skip_space(code, end)] == ')';
}

} // namespace

std::expected<void, core::Error> check_material_source(std::string_view text,
                                                       std::span<const std::string> modules) {
    const std::string code = mask(text);
    const std::vector<std::string_view> types = declared_types(code);
    for (std::size_t i = 0; i < code.size();) {
        const char c = code[i];
        if (c == '*') {
            if (auto why = pointer_star(code, i, types)) return at(text, i, *why);
            ++i;
            continue;
        }
        if (c == '&') {
            if (i + 1 < code.size() && (code[i + 1] == '&' || code[i + 1] == '=')) {
                i += 2; // `&&`, `&=`
                continue;
            }
            if (operand_position(code, i)) {
                return at(text, i, "taking an address (`&x`) is not allowed in material shaders");
            }
            ++i;
            continue;
        }
        if (c == '-' && i + 1 < code.size() && code[i + 1] == '>') {
            // `p->field` (a value before it); `func f() -> T` is a return type.
            const char p = prev_char(code, i);
            if (ident_char(p) || p == ']') {
                return at(text, i,
                          "member access through a pointer (`->`) is not allowed in material "
                          "shaders");
            }
            i += 2;
            continue;
        }
        if (c == '#') {
            return at(text, i,
                      "preprocessor directives (#include, #define, ...) are not allowed in "
                      "material shaders");
        }
        if (c == '\\') {
            return at(text, i, "a backslash is only allowed inside a string literal");
        }
        if (c == '\'') {
            // Character literals are never needed in a material, and a quote
            // inside one ('"') would otherwise open a string here that hides
            // what follows from this check.
            return at(text, i, "character literals ('x') are not allowed in material shaders");
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
        // `__include`, `__import` and any other reserved `__` spelling (Slang
        // grows new ones); `__exported import` is the one a material may use.
        if (word.starts_with("__") && word != "__exported") {
            return at(text, i, "`" + std::string(word) + "` is not allowed in material shaders");
        }
        for (const ForbiddenWord& f : k_forbidden_words) {
            if (word == f.word) return at(text, i, std::string(f.why));
        }
        if (word == "for" && for_without_condition(code, end)) {
            return at(text, i,
                      "a `for` loop without a condition is not allowed in material shaders: " +
                          std::string(k_bounded_loops));
        }
        if (word == "while" && while_constant_true(code, end)) {
            return at(text, i,
                      "`while (true)` is not allowed in material shaders: " +
                          std::string(k_bounded_loops));
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
