#pragma once

// Binary glTF 2.0 (.glb) serialisation of an asset-gen Model: one scene, one
// node, one mesh with a primitive per material (POSITION + NORMAL, indices),
// metallic-roughness factor materials. The output is byte-for-byte
// deterministic for a given Model (sorted JSON keys, fixed layout).

#include <cstddef>
#include <vector>

#include "asset-gen/mesh_builder.hpp"

namespace quat::asset_gen {

[[nodiscard]] std::vector<std::byte> write_glb(const Model& model);

} // namespace quat::asset_gen
