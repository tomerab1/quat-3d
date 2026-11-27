// Vertex attribute quantisation (KHR_mesh_quantization) and meshopt filter
// encoding of the optimised streams into new buffer views.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <string_view>

#include <meshoptimizer.h>

#include "cook/internal.hpp"

namespace engine::cook::detail {

namespace {

using fastgltf::ComponentType;
using Filter = fastgltf::MeshoptCompressionFilter;

template <typename T>
void put(std::vector<std::byte>& out, std::size_t at, T value) {
    std::memcpy(out.data() + at, &value, sizeof(T));
}

[[nodiscard]] bool starts_with(std::string_view s, std::string_view prefix) {
    return s.substr(0, prefix.size()) == prefix;
}

[[nodiscard]] bool all_unit_range(const std::vector<float>& v) {
    return std::all_of(v.begin(), v.end(), [](float x) { return x >= 0.0F && x <= 1.0F; });
}

// Points `accessor` at a new pending view.
void bind(fastgltf::Asset& asset, std::size_t accessor, PendingView view, ComponentType type,
          bool normalized, std::size_t count, GeometryResult& out) {
    fastgltf::Accessor& acc = asset.accessors[accessor];
    acc.componentType = type;
    acc.normalized = normalized;
    acc.count = count;
    acc.byteOffset = 0;
    acc.sparse.reset();
    acc.min.reset();
    acc.max.reset();
    out.bytes_after += view.raw.size();
    out.accessor_view[accessor] = out.views.size();
    out.views.push_back(std::move(view));
}

[[nodiscard]] std::int32_t snorm(float v, int bits) {
    const float scale = static_cast<float>((1 << (bits - 1)) - 1);
    return static_cast<std::int32_t>(std::lround(std::clamp(v, -1.0F, 1.0F) * scale));
}

// Unit vectors (normals; tangents with w = handedness): octahedral filter
// when compressing, plain normalised integers otherwise.
void emit_direction(fastgltf::Asset& asset, const Stream& s, std::size_t n,
                    const MeshCookOptions& o, GeometryResult& out) {
    const int bits = o.normal_bits >= 16 ? 16 : 8;
    const std::size_t comp_bytes = bits == 16 ? 2 : 1;
    PendingView v;
    v.count = n;
    v.stride = 4 * comp_bytes;
    v.raw.resize(n * v.stride);
    std::vector<float> xyzw(n * 4);
    for (std::size_t i = 0; i < n; ++i) {
        float len = 0;
        for (std::size_t c = 0; c < 3; ++c) len += s.values[i * s.components + c] * s.values[i * s.components + c];
        len = len > 0 ? std::sqrt(len) : 1.0F;
        for (std::size_t c = 0; c < 3; ++c) xyzw[i * 4 + c] = s.values[i * s.components + c] / len;
        xyzw[i * 4 + 3] = s.components == 4 ? (s.values[i * 4 + 3] < 0 ? -1.0F : 1.0F) : 0.0F;
    }
    if (o.compress) {
        v.filter = Filter::Octahedral;
        v.filtered.resize(v.raw.size());
        meshopt_encodeFilterOct(v.filtered.data(), n, v.stride, bits, xyzw.data());
        v.raw = v.filtered;
        meshopt_decodeFilterOct(v.raw.data(), n, v.stride);
    } else {
        for (std::size_t i = 0; i < n * 4; ++i) {
            const std::int32_t q = snorm(xyzw[i], bits);
            if (bits == 16) {
                put(v.raw, i * 2, static_cast<std::int16_t>(q));
            } else {
                put(v.raw, i, static_cast<std::int8_t>(q));
            }
        }
    }
    bind(asset, s.accessor, std::move(v), bits == 16 ? ComponentType::Short : ComponentType::Byte,
         true, n, out);
    out.quantized = true;
}

// POSITION: float, exponent filter when compressing; min/max of the values
// the loader will decode.
void emit_position(fastgltf::Asset& asset, const Stream& s, std::size_t n,
                   const MeshCookOptions& o, GeometryResult& out) {
    PendingView v;
    v.count = n;
    v.stride = 12;
    v.raw.resize(n * 12);
    if (o.compress && o.quantize) {
        v.filter = Filter::Exponential;
        v.filtered.resize(v.raw.size());
        meshopt_encodeFilterExp(v.filtered.data(), n, 12, std::clamp(o.position_bits, 1, 24),
                                s.values.data(), meshopt_EncodeExpSharedComponent);
        v.raw = v.filtered;
        meshopt_decodeFilterExp(v.raw.data(), n, 12);
    } else {
        std::memcpy(v.raw.data(), s.values.data(), v.raw.size());
    }
    double mn[3] = {std::numeric_limits<double>::max(), std::numeric_limits<double>::max(),
                    std::numeric_limits<double>::max()};
    double mx[3] = {std::numeric_limits<double>::lowest(), std::numeric_limits<double>::lowest(),
                    std::numeric_limits<double>::lowest()};
    for (std::size_t i = 0; i < n; ++i) {
        float p[3];
        std::memcpy(p, v.raw.data() + i * 12, 12);
        for (int c = 0; c < 3; ++c) {
            mn[c] = std::min(mn[c], static_cast<double>(p[c]));
            mx[c] = std::max(mx[c], static_cast<double>(p[c]));
        }
    }
    const std::size_t accessor = s.accessor;
    bind(asset, accessor, std::move(v), ComponentType::Float, false, n, out);
    auto lo = fastgltf::AccessorBoundsArray::ForType<double>(3);
    auto hi = fastgltf::AccessorBoundsArray::ForType<double>(3);
    for (std::size_t c = 0; c < 3; ++c) {
        lo.set<double>(c, n > 0 ? mn[c] : 0.0);
        hi.set<double>(c, n > 0 ? mx[c] : 0.0);
    }
    asset.accessors[accessor].min = std::move(lo);
    asset.accessors[accessor].max = std::move(hi);
}

// Values in [0, 1] as normalised unsigned 16-bit (UVs, colours; core glTF).
void emit_unorm16(fastgltf::Asset& asset, const Stream& s, std::size_t n, GeometryResult& out) {
    PendingView v;
    v.count = n;
    v.stride = (s.components * 2 + 3) / 4 * 4;
    v.raw.resize(n * v.stride);
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t c = 0; c < s.components; ++c) {
            const float x = s.values[i * s.components + c];
            put(v.raw, i * v.stride + c * 2, static_cast<std::uint16_t>(std::lround(x * 65535.0F)));
        }
    }
    bind(asset, s.accessor, std::move(v), ComponentType::UnsignedShort, true, n, out);
}

// Skin weights as normalised bytes whose four values sum to exactly 255.
void emit_weights(fastgltf::Asset& asset, const Stream& s, std::size_t n, GeometryResult& out) {
    PendingView v;
    v.count = n;
    v.stride = 4;
    v.raw.resize(n * 4);
    for (std::size_t i = 0; i < n; ++i) {
        const float* w = &s.values[i * 4];
        const float sum = std::max(w[0] + w[1] + w[2] + w[3], 1e-8F);
        int q[4];
        int total = 0;
        for (int c = 0; c < 4; ++c) {
            q[c] = static_cast<int>(std::lround(std::max(w[c], 0.0F) / sum * 255.0F));
            total += q[c];
        }
        const int largest = static_cast<int>(std::max_element(q, q + 4) - q);
        q[largest] = std::clamp(q[largest] + 255 - total, 0, 255);
        for (int c = 0; c < 4; ++c) put(v.raw, i * 4 + static_cast<std::size_t>(c), static_cast<std::uint8_t>(q[c]));
    }
    bind(asset, s.accessor, std::move(v), ComponentType::UnsignedByte, true, n, out);
}

void emit_joints(fastgltf::Asset& asset, const Stream& s, std::size_t n, GeometryResult& out) {
    const float max_joint = s.values.empty() ? 0.0F : *std::max_element(s.values.begin(), s.values.end());
    const bool wide = max_joint > 255.0F;
    PendingView v;
    v.count = n;
    v.stride = wide ? 8 : 4;
    v.raw.resize(n * v.stride);
    for (std::size_t i = 0; i < n * 4; ++i) {
        const auto j = static_cast<std::uint32_t>(std::max(s.values[i], 0.0F));
        if (wide) {
            put(v.raw, i * 2, static_cast<std::uint16_t>(j));
        } else {
            put(v.raw, i, static_cast<std::uint8_t>(j));
        }
    }
    bind(asset, s.accessor, std::move(v),
         wide ? ComponentType::UnsignedShort : ComponentType::UnsignedByte, false, n, out);
}

void emit_float(fastgltf::Asset& asset, const Stream& s, std::size_t n, GeometryResult& out) {
    PendingView v;
    v.count = n;
    v.stride = s.components * 4;
    v.raw.resize(n * v.stride);
    std::memcpy(v.raw.data(), s.values.data(), v.raw.size());
    bind(asset, s.accessor, std::move(v), ComponentType::Float, false, n, out);
}

} // namespace

void emit_stream(fastgltf::Asset& asset, const Stream& s, std::size_t n, const MeshCookOptions& o,
                 GeometryResult& out) {
    if (s.name == "POSITION") {
        emit_position(asset, s, n, o, out);
    } else if (o.quantize && (s.name == "NORMAL" || s.name == "TANGENT")) {
        emit_direction(asset, s, n, o, out);
    } else if (starts_with(s.name, "JOINTS_") && s.components == 4) {
        emit_joints(asset, s, n, out);
    } else if (o.quantize && starts_with(s.name, "WEIGHTS_") && s.components == 4) {
        emit_weights(asset, s, n, out);
    } else if (o.quantize && (starts_with(s.name, "TEXCOORD_") || starts_with(s.name, "COLOR_")) &&
               all_unit_range(s.values)) {
        emit_unorm16(asset, s, n, out);
    } else {
        emit_float(asset, s, n, out);
    }
}

void emit_indices(fastgltf::Asset& asset, std::size_t accessor,
                  std::span<const std::uint32_t> indices, std::size_t vertex_count,
                  const MeshCookOptions& /*options*/, GeometryResult& out) {
    const bool wide = vertex_count > 65535;
    PendingView v;
    v.vertex = false;
    v.count = indices.size();
    v.stride = wide ? 4 : 2;
    v.mode = fastgltf::MeshoptCompressionMode::Triangles;
    v.raw.resize(indices.size() * v.stride);
    for (std::size_t i = 0; i < indices.size(); ++i) {
        if (wide) {
            put(v.raw, i * 4, indices[i]);
        } else {
            put(v.raw, i * 2, static_cast<std::uint16_t>(indices[i]));
        }
    }
    fastgltf::Accessor& acc = asset.accessors[accessor];
    acc.type = fastgltf::AccessorType::Scalar;
    bind(asset, accessor, std::move(v),
         wide ? ComponentType::UnsignedInt : ComponentType::UnsignedShort, false, indices.size(),
         out);
}

} // namespace engine::cook::detail
