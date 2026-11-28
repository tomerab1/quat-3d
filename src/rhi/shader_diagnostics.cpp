#include "engine/rhi/shader_diagnostics.hpp"

#include <array>
#include <cctype>
#include <optional>

namespace engine::rhi {

namespace {

[[nodiscard]] bool is_digit(char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; }

// Parses a run of decimal digits at the start of `s` (advances it).
[[nodiscard]] std::optional<std::uint32_t> take_number(std::string_view& s) {
    std::size_t n = 0;
    std::uint64_t value = 0;
    while (n < s.size() && is_digit(s[n]) && n < 9) {
        value = value * 10 + static_cast<std::uint64_t>(s[n] - '0');
        ++n;
    }
    if (n == 0) return std::nullopt;
    s.remove_prefix(n);
    return static_cast<std::uint32_t>(value);
}

// "file(line): severity [code]: message" -> diagnostic without column.
[[nodiscard]] std::optional<ShaderDiagnostic> parse_header(std::string_view line) {
    const std::size_t close = line.find("): ");
    if (close == std::string_view::npos) return std::nullopt;
    const std::size_t open = line.rfind('(', close);
    if (open == std::string_view::npos) return std::nullopt;
    std::string_view number = line.substr(open + 1, close - open - 1);
    const auto line_no = take_number(number);
    if (!line_no || !number.empty()) return std::nullopt;

    std::string_view rest = line.substr(close + 3);
    static constexpr std::array<std::string_view, 5> severities{
        "fatal error", "internal error", "error", "warning", "note"};
    for (const std::string_view severity : severities) {
        if (!rest.starts_with(severity)) continue;
        std::string_view after = rest.substr(severity.size());
        ShaderDiagnostic d;
        if (after.starts_with(' ')) {
            after.remove_prefix(1);
            const auto code = take_number(after);
            if (!code) continue;
            d.code = *code;
        }
        if (!after.starts_with(':')) continue;
        after.remove_prefix(1);
        while (after.starts_with(' ')) after.remove_prefix(1);
        d.file = std::string(line.substr(0, open));
        d.line = *line_no;
        d.severity = std::string(severity);
        d.message = std::string(after);
        return d;
    }
    return std::nullopt;
}

// A caret line: spaces, then '^' followed by '~' / '^' only.
[[nodiscard]] std::optional<std::uint32_t> caret_column(std::string_view line) {
    const std::size_t caret = line.find('^');
    if (caret == std::string_view::npos) return std::nullopt;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        const bool ok = i < caret ? (c == ' ' || c == '\t') : (c == '^' || c == '~' || c == ' ');
        if (!ok) return std::nullopt;
    }
    return static_cast<std::uint32_t>(caret + 1);
}

} // namespace

std::vector<ShaderDiagnostic> parse_slang_diagnostics(std::string_view output) {
    std::vector<std::string_view> lines;
    while (!output.empty()) {
        const std::size_t end = output.find('\n');
        std::string_view line = output.substr(0, end);
        if (line.ends_with('\r')) line.remove_suffix(1);
        lines.push_back(line);
        if (end == std::string_view::npos) break;
        output.remove_prefix(end + 1);
    }

    std::vector<ShaderDiagnostic> out;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        auto d = parse_header(lines[i]);
        if (!d) continue;
        // Echoed source line + caret line (absent for link errors).
        if (i + 2 < lines.size() && !parse_header(lines[i + 1])) {
            if (const auto column = caret_column(lines[i + 2])) {
                d->column = *column;
                d->source_line = std::string(lines[i + 1]);
                i += 2;
            }
        }
        out.push_back(std::move(*d));
    }
    return out;
}

} // namespace engine::rhi
