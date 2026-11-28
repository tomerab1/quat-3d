// Cooked shaders carry explicit layout only where Vulkan allows it (#136).
//
// Since SPIR-V 1.6 / Vulkan 1.4 an explicit-layout decoration (ArrayStride,
// MatrixStride, member Offset) is valid only on types used by storage classes
// that have a layout -- Uniform, StorageBuffer, PushConstant,
// PhysicalStorageBuffer... A Function, Private or Workgroup variable whose
// type (or any array element / struct member of it) carries one is invalid:
// VUID-StandaloneSpirv-None-10684, reported by the validation layer of Vulkan
// SDK 1.4.321+ and spirv-val v2025.1+. Workgroup memory may be laid out
// explicitly only with WorkgroupMemoryExplicitLayoutKHR. Slang 2025.6.1
// reused one strided array type for buffers, locals and groupshared arrays;
// drivers accepted it, the validation layer did not (fixed upstream by
// shader-slang/slang#7527, first in 2025.11.1; the engine pins 2025.12).
//
// CI also runs spirv-val --target-env vulkan1.3 on every cooked .spv
// (ENGINE_VALIDATE_SHADERS); this walk runs wherever the tests run, without
// SPIRV-Tools, and names the offending variable.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <gtest/gtest.h>

namespace {

constexpr std::uint32_t k_magic = 0x07230203U;

enum Op : std::uint32_t {
    op_name = 5,
    op_capability = 17,
    op_type_int = 21,
    op_type_float = 22,
    op_type_vector = 23,
    op_type_matrix = 24,
    op_type_array = 28,
    op_type_runtime_array = 29,
    op_type_struct = 30,
    op_type_pointer = 32,
    op_constant = 43,
    op_variable = 59,
    op_decorate = 71,
    op_member_decorate = 72,
};
constexpr std::uint32_t k_array_stride = 6;
constexpr std::uint32_t k_matrix_stride = 7;
constexpr std::uint32_t k_offset = 35;
constexpr std::uint32_t k_workgroup_explicit_layout = 4428; // WorkgroupMemoryExplicitLayoutKHR

constexpr std::uint32_t k_workgroup = 4;
constexpr std::uint32_t k_private = 6;
constexpr std::uint32_t k_function = 7;
constexpr std::uint32_t k_storage_buffer = 12;
constexpr std::uint32_t k_physical = 5349; // PhysicalStorageBuffer

[[nodiscard]] const char* storage_name(std::uint32_t storage) {
    switch (storage) {
    case k_workgroup: return "Workgroup";
    case k_private: return "Private";
    case k_function: return "Function";
    default: return "?";
    }
}

[[nodiscard]] const char* decoration_name(std::uint32_t decoration) {
    switch (decoration) {
    case k_array_stride: return "ArrayStride";
    case k_matrix_stride: return "MatrixStride";
    case k_offset: return "Offset";
    default: return "?";
    }
}

// "<variable> (<storage class>): <type> has <decoration>" for every variable
// in a storage class without explicit layout whose type tree is decorated.
// A malformed module yields nothing (the loader rejects it anyway).
[[nodiscard]] std::vector<std::string> invalid_explicit_layouts(
    const std::vector<std::uint32_t>& words) {
    if (words.size() < 5 || words[0] != k_magic) return {};

    std::unordered_map<std::uint32_t, std::string>                names;
    std::unordered_map<std::uint32_t, std::uint32_t>              decorated; // type -> decoration
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> children;  // composite -> parts
    std::unordered_map<std::uint32_t, std::uint32_t>              pointee;   // pointer type -> type
    struct Variable {
        std::uint32_t id, storage, type;
    };
    std::vector<Variable> variables;
    bool                  workgroup_layout = false;

    for (std::size_t i = 5; i < words.size();) {
        const std::uint32_t count = words[i] >> 16U;
        if (count == 0 || i + count > words.size()) return {};
        const std::uint32_t  op = words[i] & 0xFFFFU;
        const std::uint32_t* o = &words[i + 1]; // operands
        const std::size_t    n = count - 1;
        switch (op) {
        case op_name:
            if (n >= 2) {
                const auto* text = reinterpret_cast<const char*>(o + 1);
                names[o[0]] = std::string(text, strnlen(text, (n - 1) * 4U));
            }
            break;
        case op_capability:
            if (n >= 1 && o[0] == k_workgroup_explicit_layout) workgroup_layout = true;
            break;
        case op_decorate:
            if (n >= 2 && (o[1] == k_array_stride || o[1] == k_matrix_stride)) {
                decorated.emplace(o[0], o[1]);
            }
            break;
        case op_member_decorate:
            if (n >= 3 && (o[2] == k_offset || o[2] == k_matrix_stride)) {
                decorated.emplace(o[0], o[2]);
            }
            break;
        case op_type_array:
        case op_type_runtime_array:
            if (n >= 2) children[o[0]] = {o[1]};
            break;
        case op_type_struct:
            if (n >= 1) children[o[0]] = std::vector<std::uint32_t>(o + 1, o + n);
            break;
        case op_type_pointer:
            if (n >= 3) pointee[o[0]] = o[2];
            break;
        case op_variable:
            if (n >= 3) variables.push_back({o[1], o[2], o[0]});
            break;
        default: break;
        }
        i += count;
    }

    const auto label = [&](std::uint32_t id) {
        const auto it = names.find(id);
        return it != names.end() && !it->second.empty() ? it->second : "%" + std::to_string(id);
    };

    std::vector<std::string> bad;
    for (const Variable& v : variables) {
        const bool layout_free = v.storage == k_function || v.storage == k_private ||
                                 (v.storage == k_workgroup && !workgroup_layout);
        const auto ptr = pointee.find(v.type);
        if (!layout_free || ptr == pointee.end()) continue;
        // Walk arrays and struct members; a pointer member's target lives in
        // its own storage class, so the walk stops there.
        std::vector<std::uint32_t>        stack{ptr->second};
        std::unordered_set<std::uint32_t> seen;
        while (!stack.empty()) {
            const std::uint32_t type = stack.back();
            stack.pop_back();
            if (!seen.insert(type).second) continue;
            if (const auto d = decorated.find(type); d != decorated.end()) {
                bad.push_back(label(v.id) + " (" + storage_name(v.storage) + "): " + label(type) +
                              " has " + decoration_name(d->second));
                break;
            }
            if (const auto c = children.find(type); c != children.end()) {
                stack.insert(stack.end(), c->second.begin(), c->second.end());
            }
        }
    }
    return bad;
}

// Minimal SPIR-V module builder for the synthetic cases.
class Module {
public:
    std::uint32_t type(std::uint32_t op, std::initializer_list<std::uint32_t> operands) {
        const std::uint32_t id = next_++;
        emit(op, {id}, operands);
        return id;
    }
    std::uint32_t vec4() {
        if (vec4_ == 0) vec4_ = type(op_type_vector, {type(op_type_float, {32}), 4});
        return vec4_;
    }
    std::uint32_t array(std::uint32_t element, std::uint32_t length) {
        const std::uint32_t u32 = type(op_type_int, {32, 0});
        const std::uint32_t len = next_++;
        emit(op_constant, {u32, len}, {length});
        return type(op_type_array, {element, len});
    }
    std::uint32_t ptr(std::uint32_t storage, std::uint32_t pointee) {
        return type(op_type_pointer, {storage, pointee});
    }
    std::uint32_t variable(std::uint32_t storage, std::uint32_t pointee) {
        const std::uint32_t id = next_++;
        emit(op_variable, {ptr(storage, pointee), id}, {storage});
        return id;
    }
    void stride(std::uint32_t type, std::uint32_t bytes) {
        emit(op_decorate, {type, k_array_stride}, {bytes});
    }
    void offset(std::uint32_t strukt, std::uint32_t member, std::uint32_t bytes) {
        emit(op_member_decorate, {strukt, member, k_offset}, {bytes});
    }
    void capability(std::uint32_t cap) { emit(op_capability, {cap}, {}); }

    [[nodiscard]] std::vector<std::uint32_t> words() const {
        std::vector<std::uint32_t> out{k_magic, 0x00010600U, 0U, next_, 0U};
        out.insert(out.end(), body_.begin(), body_.end());
        return out;
    }

private:
    void emit(std::uint32_t op, std::initializer_list<std::uint32_t> head,
              std::initializer_list<std::uint32_t> rest) {
        const auto count = static_cast<std::uint32_t>(1 + head.size() + rest.size());
        body_.push_back((count << 16U) | op);
        body_.insert(body_.end(), head);
        body_.insert(body_.end(), rest);
    }

    std::vector<std::uint32_t> body_;
    std::uint32_t              next_ = 1;
    std::uint32_t              vec4_ = 0;
};

// The 2025.6.1 shape: one strided float4[3] shared by a buffer and a local.
TEST(SpirvExplicitLayout, FlagsAStridedArrayUsedByAFunctionVariable) {
    Module m;
    const std::uint32_t arr = m.array(m.vec4(), 3);
    m.stride(arr, 16);
    const std::uint32_t block = m.type(op_type_struct, {arr});
    m.offset(block, 0, 0);
    m.variable(k_storage_buffer, block);
    m.variable(k_function, arr);
    EXPECT_EQ(invalid_explicit_layouts(m.words()).size(), 1U);
}

TEST(SpirvExplicitLayout, FlagsAStridedGroupsharedArray) {
    Module m;
    const std::uint32_t arr = m.array(m.vec4(), 64);
    m.stride(arr, 16);
    m.variable(k_workgroup, arr);
    EXPECT_EQ(invalid_explicit_layouts(m.words()).size(), 1U);
}

TEST(SpirvExplicitLayout, AcceptsExplicitWorkgroupLayoutWithItsCapability) {
    Module m;
    m.capability(k_workgroup_explicit_layout);
    const std::uint32_t arr = m.array(m.vec4(), 64);
    m.stride(arr, 16);
    m.variable(k_workgroup, arr);
    EXPECT_TRUE(invalid_explicit_layouts(m.words()).empty());
}

TEST(SpirvExplicitLayout, FlagsAnOffsetOnAStructNestedInALocalArray) {
    Module m;
    const std::uint32_t inner = m.type(op_type_struct, {m.vec4()});
    m.offset(inner, 0, 0);
    m.variable(k_private, m.array(inner, 4));
    EXPECT_EQ(invalid_explicit_layouts(m.words()).size(), 1U);
}

// The fixed shape: separate plain and strided types per storage class.
TEST(SpirvExplicitLayout, AcceptsSeparateTypesPerStorageClass) {
    Module m;
    const std::uint32_t strided = m.array(m.vec4(), 3);
    m.stride(strided, 16);
    const std::uint32_t block = m.type(op_type_struct, {strided});
    m.offset(block, 0, 0);
    m.variable(k_storage_buffer, block);
    m.variable(k_function, m.array(m.vec4(), 3));
    m.variable(k_workgroup, m.array(m.vec4(), 64));
    EXPECT_TRUE(invalid_explicit_layouts(m.words()).empty());
}

// A local holding a device address: the pointee is PhysicalStorageBuffer
// memory, which is laid out explicitly.
TEST(SpirvExplicitLayout, StopsAtPointerMembers) {
    Module m;
    const std::uint32_t record = m.type(op_type_struct, {m.vec4()});
    m.offset(record, 0, 0);
    m.variable(k_function, m.type(op_type_struct, {m.ptr(k_physical, record)}));
    EXPECT_TRUE(invalid_explicit_layouts(m.words()).empty());
}

TEST(SpirvExplicitLayout, MalformedModulesYieldNothing) {
    Module m;
    const std::uint32_t arr = m.array(m.vec4(), 3);
    m.stride(arr, 16);
    m.variable(k_function, arr);
    std::vector<std::uint32_t> bad_magic = m.words();
    bad_magic[0] = 0;
    EXPECT_TRUE(invalid_explicit_layouts(bad_magic).empty());
    std::vector<std::uint32_t> truncated = m.words();
    truncated.pop_back();
    EXPECT_TRUE(invalid_explicit_layouts(truncated).empty());
}

// No cooked pass or material shader puts an explicit layout on a Function,
// Private or Workgroup variable's type.
TEST(SpirvExplicitLayout, NoCookedShaderHasExplicitLayoutOnLayoutFreeVariables) {
    const std::filesystem::path dir = QUAT_COOKED_SHADER_DIR;
    std::error_code             ec;
    if (!std::filesystem::is_directory(dir, ec)) GTEST_SKIP() << "no cooked shaders at " << dir;
    int checked = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(dir, ec)) {
        if (entry.path().extension() != ".spv") continue;
        std::ifstream     in(entry.path(), std::ios::binary);
        std::vector<char> bytes((std::istreambuf_iterator<char>(in)), {});
        ASSERT_EQ(bytes.size() % 4, 0U) << entry.path();
        std::vector<std::uint32_t> words(bytes.size() / 4);
        std::memcpy(words.data(), bytes.data(), bytes.size());
        for (const std::string& v : invalid_explicit_layouts(words)) {
            ADD_FAILURE() << entry.path().filename() << ": " << v
                          << " (VUID-StandaloneSpirv-None-10684; issue #136: the Slang "
                             "release must emit separate types per storage class)";
        }
        ++checked;
    }
    EXPECT_GT(checked, 0);
}

} // namespace
