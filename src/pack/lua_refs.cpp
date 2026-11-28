#include "engine/pack/lua_refs.hpp"

#include <cstddef>
#include <optional>

namespace engine::pack {

namespace {

[[nodiscard]] bool is_ident_start(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
[[nodiscard]] bool is_ident(char c) { return is_ident_start(c) || (c >= '0' && c <= '9'); }

class Lexer {
public:
    explicit Lexer(std::string_view s) : s_(s) {}

    std::vector<LuaLiteral> run() {
        std::vector<LuaLiteral> out;
        // Tracks `require` [ "(" ] <string>: 0 none, 1 saw require, 2 saw "(".
        int require_state = 0;
        while (i_ < s_.size()) {
            const char c = s_[i_];
            if (c == '\n') {
                ++line_;
                ++i_;
                continue;
            }
            if (c == ' ' || c == '\t' || c == '\r') {
                ++i_;
                continue;
            }
            if (c == '-' && peek(1) == '-') {
                i_ += 2;
                if (peek(0) == '[') {
                    if (const auto level = long_bracket_level(); level) {
                        if (!skip_long(*level, nullptr)) return out;
                        continue;
                    }
                }
                while (i_ < s_.size() && s_[i_] != '\n') ++i_;
                continue;
            }
            if (c == '"' || c == '\'') {
                const std::uint32_t at = line_;
                std::string text;
                if (!short_string(c, text)) return out;
                out.push_back({std::move(text), at, require_state != 0});
                require_state = 0;
                continue;
            }
            if (c == '[') {
                if (const auto level = long_bracket_level(); level) {
                    const std::uint32_t at = line_;
                    std::string text;
                    if (!skip_long(*level, &text)) return out;
                    out.push_back({std::move(text), at, require_state != 0});
                    require_state = 0;
                    continue;
                }
            }
            if (is_ident_start(c)) {
                const std::size_t start = i_;
                while (i_ < s_.size() && is_ident(s_[i_])) ++i_;
                require_state = s_.substr(start, i_ - start) == "require" ? 1 : 0;
                continue;
            }
            require_state = (c == '(' && require_state == 1) ? 2 : 0;
            ++i_;
        }
        return out;
    }

private:
    [[nodiscard]] char peek(std::size_t ahead) const {
        return i_ + ahead < s_.size() ? s_[i_ + ahead] : '\0';
    }

    // At '[': the level of a long bracket "[==[" (number of '='), if one
    // opens here.
    [[nodiscard]] std::optional<std::size_t> long_bracket_level() const {
        std::size_t j = i_ + 1;
        std::size_t level = 0;
        while (j < s_.size() && s_[j] == '=') {
            ++level;
            ++j;
        }
        if (j < s_.size() && s_[j] == '[') return level;
        return std::nullopt;
    }

    // Consumes a long string/comment of `level` starting at '['. A leading
    // newline is dropped, as Lua does.
    bool skip_long(std::size_t level, std::string* text) {
        i_ += level + 2;
        if (peek(0) == '\r') ++i_;
        if (peek(0) == '\n') {
            ++line_;
            ++i_;
        }
        while (i_ < s_.size()) {
            if (s_[i_] == ']') {
                std::size_t j = i_ + 1;
                std::size_t eq = 0;
                while (j < s_.size() && s_[j] == '=') {
                    ++eq;
                    ++j;
                }
                if (eq == level && j < s_.size() && s_[j] == ']') {
                    i_ = j + 1;
                    return true;
                }
            }
            if (s_[i_] == '\n') ++line_;
            if (text != nullptr) text->push_back(s_[i_]);
            ++i_;
        }
        return false;
    }

    bool short_string(char quote, std::string& text) {
        ++i_;
        while (i_ < s_.size()) {
            const char c = s_[i_];
            if (c == quote) {
                ++i_;
                return true;
            }
            if (c == '\n') return false; // unfinished string
            if (c != '\\') {
                text.push_back(c);
                ++i_;
                continue;
            }
            const char e = peek(1);
            i_ += 2;
            switch (e) {
            case 'n': text.push_back('\n'); break;
            case 't': text.push_back('\t'); break;
            case 'r': text.push_back('\r'); break;
            case 'a': text.push_back('\a'); break;
            case 'b': text.push_back('\b'); break;
            case 'f': text.push_back('\f'); break;
            case 'v': text.push_back('\v'); break;
            case '\\': text.push_back('\\'); break;
            case '"': text.push_back('"'); break;
            case '\'': text.push_back('\''); break;
            case '\n': text.push_back('\n'); ++line_; break;
            case 'z': // skip following whitespace
                while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\n' || s_[i_] == '\t' ||
                                          s_[i_] == '\r')) {
                    if (s_[i_] == '\n') ++line_;
                    ++i_;
                }
                break;
            default:
                // \ddd, \xXX, \u{...}: not path material; keep the raw text.
                text.push_back('\\');
                text.push_back(e);
                break;
            }
        }
        return false;
    }

    std::string_view s_;
    std::size_t      i_ = 0;
    std::uint32_t    line_ = 1;
};

} // namespace

std::vector<LuaLiteral> lua_string_literals(std::string_view source) {
    return Lexer(source).run();
}

} // namespace engine::pack
