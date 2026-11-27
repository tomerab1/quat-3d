#include "engine/asset/cube_lut.hpp"

#include <array>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>

#include "engine/core/utf8_path.hpp"

namespace engine::asset {

namespace {

[[nodiscard]] std::unexpected<core::Error> fail(std::string what) {
    return std::unexpected(core::Error{std::move(what)});
}

[[nodiscard]] bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r'; }

// Splits a line into whitespace-separated tokens (at most `max` + 1, so an
// overlong line is detected without storing it).
struct Tokens {
    std::array<std::string_view, 5> items{};
    std::size_t                     count = 0;
};

[[nodiscard]] Tokens split(std::string_view line) {
    Tokens out;
    std::size_t i = 0;
    while (i < line.size() && out.count < out.items.size()) {
        while (i < line.size() && is_space(line[i])) ++i;
        if (i >= line.size()) break;
        const std::size_t start = i;
        while (i < line.size() && !is_space(line[i])) ++i;
        out.items[out.count++] = line.substr(start, i - start);
    }
    return out;
}

// A finite decimal number spanning the whole token (strtof on a bounded,
// NUL-terminated copy: the text is never read past the token).
[[nodiscard]] bool parse_float(std::string_view token, float& out) {
    if (token.empty() || token.size() > 63) return false;
    std::array<char, 64> buf{};
    for (std::size_t i = 0; i < token.size(); ++i) {
        const char c = token[i];
        const bool ok = (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '+' || c == 'e' ||
                        c == 'E';
        if (!ok) return false; // also rules out inf / nan / hex floats
        buf[i] = c;
    }
    char* end = nullptr;
    const float v = std::strtof(buf.data(), &end);
    if (end != buf.data() + token.size() || !std::isfinite(v)) return false;
    out = v;
    return true;
}

[[nodiscard]] bool parse_size(std::string_view token, std::uint32_t& out) {
    if (token.empty() || token.size() > 4) return false;
    std::uint32_t v = 0;
    for (const char c : token) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + static_cast<std::uint32_t>(c - '0');
    }
    out = v;
    return true;
}

} // namespace

std::expected<CubeLut, core::Error> parse_cube_lut(std::string_view text) {
    if (text.size() > CubeLut::k_max_file_bytes) {
        return fail("cube LUT: file larger than " + std::to_string(CubeLut::k_max_file_bytes >> 20U) +
                    " MiB");
    }
    CubeLut lut;
    std::size_t expected = 0; // entries, known once LUT_3D_SIZE is read
    std::size_t line_no = 0;
    std::size_t pos = 0;
    bool        domain_set = false;
    while (pos < text.size()) {
        std::size_t end = text.find('\n', pos);
        if (end == std::string_view::npos) end = text.size();
        const std::string_view line = text.substr(pos, end - pos);
        pos = end + 1;
        ++line_no;
        const std::string at = "cube LUT line " + std::to_string(line_no) + ": ";

        const Tokens t = split(line);
        if (t.count == 0 || t.items[0].front() == '#') continue;
        const std::string_view key = t.items[0];
        const bool data = (key.front() >= '0' && key.front() <= '9') || key.front() == '-' ||
                          key.front() == '+' || key.front() == '.';
        if (data) {
            if (lut.size == 0) return fail(at + "data before LUT_3D_SIZE");
            if (t.count != 3) return fail(at + "expected three numbers (r g b)");
            if (lut.rgb.size() / 3 >= expected) {
                return fail(at + "more than " + std::to_string(expected) + " entries");
            }
            for (std::size_t k = 0; k < 3; ++k) {
                float v = 0.0F;
                if (!parse_float(t.items[k], v)) return fail(at + "not a finite number");
                lut.rgb.push_back(v);
            }
            continue;
        }
        if (!lut.rgb.empty()) return fail(at + "keyword '" + std::string(key) + "' after data");
        if (key == "TITLE") {
            const std::size_t q0 = line.find('"');
            const std::size_t q1 = q0 == std::string_view::npos ? q0 : line.find('"', q0 + 1);
            if (q1 == std::string_view::npos) return fail(at + "TITLE needs a quoted name");
            lut.title = std::string(line.substr(q0 + 1, std::min<std::size_t>(q1 - q0 - 1, 256)));
        } else if (key == "LUT_3D_SIZE") {
            std::uint32_t n = 0;
            if (t.count != 2 || !parse_size(t.items[1], n)) return fail(at + "LUT_3D_SIZE needs one integer");
            if (lut.size != 0) return fail(at + "LUT_3D_SIZE given twice");
            if (n < CubeLut::k_min_size || n > CubeLut::k_max_size) {
                return fail(at + "LUT_3D_SIZE " + std::to_string(n) + " outside [" +
                            std::to_string(CubeLut::k_min_size) + ", " +
                            std::to_string(CubeLut::k_max_size) + "]");
            }
            lut.size = n;
            expected = static_cast<std::size_t>(n) * n * n;
            lut.rgb.reserve(expected * 3);
        } else if (key == "LUT_1D_SIZE") {
            return fail(at + "1D LUTs are not supported (expected LUT_3D_SIZE)");
        } else if (key == "DOMAIN_MIN" || key == "DOMAIN_MAX") {
            if (t.count != 4) return fail(at + std::string(key) + " needs three numbers");
            auto& dst = key == "DOMAIN_MIN" ? lut.domain_min : lut.domain_max;
            for (std::size_t k = 0; k < 3; ++k) {
                if (!parse_float(t.items[k + 1], dst[k])) return fail(at + "not a finite number");
            }
            domain_set = true;
        } else if (key == "LUT_3D_INPUT_RANGE") {
            float lo = 0.0F, hi = 0.0F;
            if (t.count != 3 || !parse_float(t.items[1], lo) || !parse_float(t.items[2], hi)) {
                return fail(at + "LUT_3D_INPUT_RANGE needs two numbers");
            }
            lut.domain_min = {lo, lo, lo};
            lut.domain_max = {hi, hi, hi};
            domain_set = true;
        } else {
            return fail(at + "unknown keyword '" + std::string(key.substr(0, 32)) + "'");
        }
    }
    if (lut.size == 0) return fail("cube LUT: no LUT_3D_SIZE");
    if (lut.rgb.size() / 3 != expected) {
        return fail("cube LUT: " + std::to_string(lut.rgb.size() / 3) + " entries, expected " +
                    std::to_string(expected) + " (LUT_3D_SIZE " + std::to_string(lut.size) + ")");
    }
    if (domain_set) {
        for (std::size_t k = 0; k < 3; ++k) {
            if (!(lut.domain_max[k] > lut.domain_min[k])) {
                return fail("cube LUT: DOMAIN_MAX must exceed DOMAIN_MIN in every channel");
            }
        }
    }
    return lut;
}

std::expected<CubeLut, core::Error> load_cube_lut(const std::filesystem::path& path) {
    const std::string name = core::path_to_utf8(path);
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec) return fail("cube LUT: cannot stat '" + name + "': " + ec.message());
    if (size > CubeLut::k_max_file_bytes) {
        return fail("cube LUT: '" + name + "' is larger than " +
                    std::to_string(CubeLut::k_max_file_bytes >> 20U) + " MiB");
    }
    // std::ifstream takes the path natively (wide on Windows).
    std::ifstream file(path, std::ios::binary);
    if (!file) return fail("cube LUT: cannot open '" + name + "'");
    std::string text(static_cast<std::size_t>(size), '\0');
    if (!file.read(text.data(), static_cast<std::streamsize>(text.size()))) {
        return fail("cube LUT: short read on '" + name + "'");
    }
    auto lut = parse_cube_lut(text);
    if (!lut) return fail("'" + name + "': " + lut.error().message);
    return lut;
}

CubeLut identity_cube_lut(std::uint32_t size) {
    CubeLut lut;
    lut.size = size;
    lut.title = "identity";
    lut.rgb.reserve(static_cast<std::size_t>(size) * size * size * 3);
    const float scale = 1.0F / static_cast<float>(size - 1);
    for (std::uint32_t b = 0; b < size; ++b) {
        for (std::uint32_t g = 0; g < size; ++g) {
            for (std::uint32_t r = 0; r < size; ++r) {
                lut.rgb.push_back(static_cast<float>(r) * scale);
                lut.rgb.push_back(static_cast<float>(g) * scale);
                lut.rgb.push_back(static_cast<float>(b) * scale);
            }
        }
    }
    return lut;
}

} // namespace engine::asset
