#pragma once

// External files of a glTF model (ROADMAP P4): the `uri`s of its buffers and
// images that are not embedded data: URIs, percent-decoded, relative to the
// model's directory. Works on .gltf (JSON) and .glb (the JSON chunk); the
// binary payload is not read.

#include <expected>
#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include "engine/core/error.hpp"

namespace engine::pack {

// URIs (decoded, as written relative to the model) of a parsed glTF JSON
// document. Fails on a malformed buffers/images array or a URI with a
// scheme other than data: (http://, file://, ...).
[[nodiscard]] std::expected<std::vector<std::string>, core::Error>
gltf_document_uris(const nlohmann::json& doc);

// Reads a .gltf / .glb file and returns its external URIs.
[[nodiscard]] std::expected<std::vector<std::string>, core::Error>
gltf_file_uris(const std::filesystem::path& path);

// RFC 3986 percent-decoding ("%20" -> ' '); fails on a malformed escape.
[[nodiscard]] std::expected<std::string, core::Error> percent_decode(std::string_view s);

} // namespace engine::pack
