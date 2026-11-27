// Triangle mesh cooking: primitives that share one vertex stream set are
// optionally simplified (mesh_simplify.cpp), optimised together (vertex
// cache, overdraw, vertex fetch), then re-emitted quantised.

#include <algorithm>
#include <map>
#include <string>
#include <string_view>

#include <fastgltf/tools.hpp>
#include <meshoptimizer.h>

#include "cook/internal.hpp"

namespace engine::cook::detail {

namespace {

// A primitive's attribute -> accessor map, sorted by name.
using AttributeKey = std::vector<std::pair<std::string, std::size_t>>;

[[nodiscard]] AttributeKey attribute_key(const fastgltf::Primitive& prim) {
    AttributeKey key;
    for (const auto& a : prim.attributes) {
        key.emplace_back(std::string(std::string_view(a.name)), a.accessorIndex);
    }
    std::sort(key.begin(), key.end());
    return key;
}

// How many references each accessor has anywhere in the asset.
[[nodiscard]] std::vector<std::size_t> accessor_uses(const fastgltf::Asset& asset) {
    std::vector<std::size_t> uses(asset.accessors.size(), 0);
    const auto use = [&](std::size_t i) {
        if (i < uses.size()) ++uses[i];
    };
    for (const fastgltf::Mesh& mesh : asset.meshes) {
        for (const fastgltf::Primitive& prim : mesh.primitives) {
            for (const auto& a : prim.attributes) use(a.accessorIndex);
            if (prim.indicesAccessor) use(*prim.indicesAccessor);
            for (const auto& target : prim.targets) {
                for (const auto& a : target) use(a.accessorIndex);
            }
        }
    }
    for (const fastgltf::Skin& skin : asset.skins) {
        if (skin.inverseBindMatrices) use(*skin.inverseBindMatrices);
    }
    for (const fastgltf::Animation& anim : asset.animations) {
        for (const auto& s : anim.samplers) {
            use(s.inputAccessor);
            use(s.outputAccessor);
        }
    }
    for (const fastgltf::Node& node : asset.nodes) {
        for (const auto& a : node.instancingAttributes) use(a.accessorIndex);
    }
    return uses;
}

[[nodiscard]] std::size_t component_count(fastgltf::AccessorType t) {
    switch (t) {
        case fastgltf::AccessorType::Scalar: return 1;
        case fastgltf::AccessorType::Vec2:   return 2;
        case fastgltf::AccessorType::Vec3:   return 3;
        case fastgltf::AccessorType::Vec4:   return 4;
        default:                             return 0;
    }
}

[[nodiscard]] std::vector<float> read_floats(const fastgltf::Asset& asset,
                                             const fastgltf::Accessor& acc, std::size_t comps) {
    std::vector<float> out(acc.count * comps);
    const auto store = [&](const auto& v, std::size_t i) {
        for (std::size_t c = 0; c < comps; ++c) out[i * comps + c] = v[c];
    };
    switch (comps) {
        case 1:
            fastgltf::iterateAccessorWithIndex<float>(
                asset, acc, [&](float v, std::size_t i) { out[i] = v; });
            break;
        case 2:
            fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec2>(asset, acc, store);
            break;
        case 3:
            fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec3>(asset, acc, store);
            break;
        default:
            fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec4>(asset, acc, store);
            break;
    }
    return out;
}

struct Group {
    std::size_t mesh = 0;
    std::vector<std::size_t> primitives; // indices into the mesh's primitives
};

// Appends a Scalar index accessor (for a primitive that had none).
[[nodiscard]] std::size_t add_index_accessor(fastgltf::Asset& asset) {
    fastgltf::Accessor acc{};
    acc.type = fastgltf::AccessorType::Scalar;
    acc.componentType = fastgltf::ComponentType::UnsignedInt;
    asset.accessors.push_back(std::move(acc));
    return asset.accessors.size() - 1;
}

[[nodiscard]] std::expected<void, core::Error>
cook_group(fastgltf::Asset& asset, const Group& g, const MeshCookOptions& o, GeometryResult& out) {
    fastgltf::Mesh& mesh = asset.meshes[g.mesh];
    const AttributeKey key = attribute_key(mesh.primitives[g.primitives.front()]);
    const fastgltf::Accessor& pos_acc = asset.accessors[
        std::find_if(key.begin(), key.end(), [](const auto& a) { return a.first == "POSITION"; })
            ->second];
    const std::size_t vertex_count = pos_acc.count;

    std::vector<Stream> streams;
    for (const auto& [name, acc_index] : key) {
        const fastgltf::Accessor& acc = asset.accessors[acc_index];
        Stream s{name, acc_index, component_count(acc.type), {}};
        s.values = read_floats(asset, acc, s.components);
        out.bytes_before += acc.count * fastgltf::getElementByteSize(acc.type, acc.componentType);
        streams.push_back(std::move(s));
    }
    const auto find_stream = [&](std::string_view name) -> Stream* {
        const auto it = std::find_if(streams.begin(), streams.end(),
                                     [&](const Stream& s) { return s.name == name; });
        return it == streams.end() ? nullptr : &*it;
    };
    Stream& positions = *find_stream("POSITION");

    std::vector<std::vector<std::uint32_t>> indices(g.primitives.size());
    for (std::size_t k = 0; k < g.primitives.size(); ++k) {
        const fastgltf::Primitive& prim = mesh.primitives[g.primitives[k]];
        std::vector<std::uint32_t>& idx = indices[k];
        if (prim.indicesAccessor) {
            const fastgltf::Accessor& acc = asset.accessors[*prim.indicesAccessor];
            out.bytes_before += acc.count * fastgltf::getComponentByteSize(acc.componentType);
            idx.reserve(acc.count);
            fastgltf::iterateAccessor<std::uint32_t>(asset, acc,
                                                     [&](std::uint32_t i) { idx.push_back(i); });
        } else {
            idx.resize(vertex_count);
            for (std::size_t i = 0; i < vertex_count; ++i) idx[i] = static_cast<std::uint32_t>(i);
        }
        if (idx.size() % 3 != 0 ||
            std::any_of(idx.begin(), idx.end(), [&](std::uint32_t i) { return i >= vertex_count; })) {
            return fail("mesh " + std::to_string(g.mesh) + " has invalid triangle indices");
        }
    }

    // Level of detail first (it may move foliage vertices: shared[v] marks
    // vertices more than one primitive references, which stay put).
    if (o.simplify_ratio < 1.0F) {
        std::vector<std::uint8_t> shared(vertex_count, 0);
        std::vector<std::uint32_t> owner(vertex_count, ~0U);
        for (std::size_t k = 0; k < indices.size(); ++k) {
            for (const std::uint32_t v : indices[k]) {
                if (owner[v] == ~0U) owner[v] = static_cast<std::uint32_t>(k);
                else if (owner[v] != k) shared[v] = 1;
            }
        }
        for (auto& idx : indices) {
            simplify_triangles(idx, positions, find_stream("NORMAL"), find_stream("TEXCOORD_0"),
                               shared, o);
        }
    }

    for (auto& idx : indices) {
        if (o.optimize && !idx.empty()) {
            meshopt_optimizeVertexCache(idx.data(), idx.data(), idx.size(), vertex_count);
            meshopt_optimizeOverdraw(idx.data(), idx.data(), idx.size(), positions.values.data(),
                                     vertex_count, sizeof(float) * 3, o.overdraw_threshold);
        }
    }

    // One vertex-fetch remap over every primitive sharing the streams (also
    // drops vertices no triangle references).
    std::size_t new_count = vertex_count;
    if (o.optimize) {
        std::vector<std::uint32_t> all;
        for (const auto& idx : indices) all.insert(all.end(), idx.begin(), idx.end());
        std::vector<std::uint32_t> remap(vertex_count);
        new_count = meshopt_optimizeVertexFetchRemap(remap.data(), all.data(), all.size(),
                                                     vertex_count);
        for (Stream& s : streams) {
            std::vector<float> moved(new_count * s.components);
            meshopt_remapVertexBuffer(moved.data(), s.values.data(), vertex_count,
                                      s.components * sizeof(float), remap.data());
            s.values = std::move(moved);
        }
        for (auto& idx : indices) {
            meshopt_remapIndexBuffer(idx.data(), idx.data(), idx.size(), remap.data());
        }
    }

    for (const Stream& s : streams) emit_stream(asset, s, new_count, o, out);

    for (std::size_t k = 0; k < g.primitives.size(); ++k) {
        fastgltf::Primitive& prim = mesh.primitives[g.primitives[k]];
        if (!prim.indicesAccessor) prim.indicesAccessor = add_index_accessor(asset);
        out.accessor_view.resize(asset.accessors.size());
        emit_indices(asset, *prim.indicesAccessor, indices[k], new_count, o, out);

        ++out.cooked;
    }
    return {};
}

} // namespace

std::expected<GeometryResult, core::Error> cook_meshes(fastgltf::Asset& asset,
                                                       const MeshCookOptions& o) {
    GeometryResult out;
    out.accessor_view.resize(asset.accessors.size());
    const std::vector<std::size_t> uses = accessor_uses(asset);

    for (std::size_t m = 0; m < asset.meshes.size(); ++m) {
        // Group this mesh's primitives by identical attribute maps.
        std::map<AttributeKey, Group> groups;
        for (std::size_t p = 0; p < asset.meshes[m].primitives.size(); ++p) {
            const fastgltf::Primitive& prim = asset.meshes[m].primitives[p];
            Group& g = groups[attribute_key(prim)];
            g.mesh = m;
            g.primitives.push_back(p);
        }
        for (auto& [key, g] : groups) {
            // Cookable when triangles without morph targets, every stream is a
            // dense float-readable accessor used only by this group, and each
            // index accessor belongs to one primitive.
            bool ok = std::any_of(key.begin(), key.end(),
                                  [](const auto& a) { return a.first == "POSITION"; });
            for (const auto& [name, acc] : key) {
                const fastgltf::Accessor& a = asset.accessors[acc];
                ok = ok && uses[acc] == g.primitives.size() && a.bufferViewIndex.has_value() &&
                     !a.sparse.has_value() && component_count(a.type) != 0 &&
                     a.count == asset.accessors[key.front().second].count;
            }
            for (const std::size_t p : g.primitives) {
                const fastgltf::Primitive& prim = asset.meshes[m].primitives[p];
                ok = ok && prim.type == fastgltf::PrimitiveType::Triangles && prim.targets.empty() &&
                     (!prim.indicesAccessor ||
                      (uses[*prim.indicesAccessor] == 1 &&
                       asset.accessors[*prim.indicesAccessor].bufferViewIndex.has_value() &&
                       !asset.accessors[*prim.indicesAccessor].sparse.has_value()));
            }
            if (!ok) {
                out.skipped += static_cast<std::uint32_t>(g.primitives.size());
                continue;
            }
            if (auto r = cook_group(asset, g, o, out); !r) return std::unexpected(r.error());
        }
    }
    out.accessor_view.resize(asset.accessors.size());
    return out;
}

} // namespace engine::cook::detail
