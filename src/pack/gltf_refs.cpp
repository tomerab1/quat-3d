#include "engine/pack/gltf_refs.hpp"

#include <array>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>

#include "engine/core/utf8_path.hpp"

namespace engine::pack {

namespace {

using nlohmann::json;

constexpr std::uintmax_t k_max_gltf_json_bytes = 256ULL << 20;
constexpr std::uint32_t  k_glb_magic = 0x46546C67; // "glTF"
constexpr std::uint32_t  k_glb_json_chunk = 0x4E4F534A; // "JSON"

[[nodiscard]] std::unexpected<core::Error> fail(std::string what) {
    return std::unexpected(core::Error{std::move(what)});
}

[[nodiscard]] int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// "scheme:" per RFC 3986 (letter, then letters/digits/+-.), at least two
// characters so a Windows drive letter is not mistaken for one.
[[nodiscard]] bool has_scheme(std::string_view uri) {
    const std::size_t colon = uri.find(':');
    if (colon == std::string_view::npos || colon < 2) return false;
    for (std::size_t i = 0; i < colon; ++i) {
        const char c = uri[i];
        const bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        if (!alpha && !(i > 0 && ((c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.'))) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::uint32_t read_u32(const std::array<char, 4>& b) {
    return static_cast<std::uint32_t>(static_cast<unsigned char>(b[0])) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(b[1])) << 8) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(b[2])) << 16) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(b[3])) << 24);
}

[[nodiscard]] std::expected<std::string, core::Error> read_glb_json(std::ifstream& in,
                                                                   std::uintmax_t file_size) {
    std::array<char, 4> word{};
    const auto next = [&]() -> bool {
        in.read(word.data(), 4);
        return in.gcount() == 4;
    };
    if (!next() || read_u32(word) != k_glb_magic) return fail("not a GLB file");
    if (!next() || read_u32(word) != 2) return fail("GLB: only version 2 is supported");
    if (!next()) return fail("GLB: truncated header");
    if (!next()) return fail("GLB: truncated chunk header");
    const std::uint32_t json_len = read_u32(word);
    if (!next() || read_u32(word) != k_glb_json_chunk) return fail("GLB: first chunk is not JSON");
    if (json_len > file_size || json_len > k_max_gltf_json_bytes) {
        return fail("GLB: JSON chunk length out of range");
    }
    std::string text(json_len, '\0');
    in.read(text.data(), static_cast<std::streamsize>(json_len));
    if (static_cast<std::uint32_t>(in.gcount()) != json_len) return fail("GLB: truncated JSON chunk");
    return text;
}

} // namespace

std::expected<std::string, core::Error> percent_decode(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '%') {
            out.push_back(s[i]);
            continue;
        }
        const int hi = i + 1 < s.size() ? hex_value(s[i + 1]) : -1;
        const int lo = i + 2 < s.size() ? hex_value(s[i + 2]) : -1;
        if (hi < 0 || lo < 0) return fail("bad %-escape in '" + std::string(s) + "'");
        out.push_back(static_cast<char>(hi * 16 + lo));
        i += 2;
    }
    if (out.find('\0') != std::string::npos) return fail("URI decodes to a NUL byte");
    return out;
}

std::expected<std::vector<std::string>, core::Error> gltf_document_uris(const json& doc) {
    if (!doc.is_object()) return fail("glTF: expected a JSON object");
    std::vector<std::string> uris;
    for (const char* array : {"buffers", "images"}) {
        const auto it = doc.find(array);
        if (it == doc.end()) continue;
        if (!it->is_array()) return fail(std::string("glTF: ") + array + ": expected an array");
        for (std::size_t i = 0; i < it->size(); ++i) {
            const json& entry = (*it)[i];
            const std::string where = std::string("glTF ") + array + "[" + std::to_string(i) + "]";
            if (!entry.is_object()) return fail(where + ": expected an object");
            const auto uri = entry.find("uri");
            if (uri == entry.end()) continue; // GLB binary chunk / bufferView image
            if (!uri->is_string()) return fail(where + ".uri: expected a string");
            const std::string& raw = uri->get_ref<const std::string&>();
            if (raw.starts_with("data:")) continue;
            if (has_scheme(raw)) {
                return fail(where + ".uri: '" + raw.substr(0, 128) +
                            "' is not a relative file (only embedded data: URIs and files next "
                            "to the model can be packaged)");
            }
            auto decoded = percent_decode(raw);
            if (!decoded) return fail(where + ".uri: " + decoded.error().message);
            uris.push_back(std::move(*decoded));
        }
    }
    return uris;
}

std::expected<std::vector<std::string>, core::Error>
gltf_file_uris(const std::filesystem::path& path) {
    const std::string name = core::path_to_utf8(path);
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec) return fail("model '" + name + "': cannot read (" + ec.message() + ")");
    std::ifstream in(path, std::ios::binary);
    if (!in) return fail("model '" + name + "': cannot open");

    std::string text;
    std::string ext = core::path_to_utf8(path.extension());
    for (char& c : ext) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    if (ext == ".glb") {
        auto t = read_glb_json(in, size);
        if (!t) return fail("model '" + name + "': " + t.error().message);
        text = std::move(*t);
    } else {
        if (size > k_max_gltf_json_bytes) return fail("model '" + name + "': file too large");
        text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    const json doc = json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (doc.is_discarded()) return fail("model '" + name + "': glTF JSON is not valid");
    auto uris = gltf_document_uris(doc);
    if (!uris) return fail("model '" + name + "': " + uris.error().message);
    return uris;
}

} // namespace engine::pack
