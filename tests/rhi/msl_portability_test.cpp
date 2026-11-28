// Cooked shaders stay translatable to Metal Shading Language (issue #103).
//
// MoltenVK converts SPIR-V to MSL with SPIRV-Cross at pipeline creation.
// SPIRV-Cross mistranslates dereferencing a buffer-device-address pointer
// that was itself read from an array element: the OpPtrAccessChain index is
// folded into the element's index, so `motion[instance].prev_positions[vertex]`
// became `motion[instance + vertex].prev_positions` (the wrong record, and a
// pointer where a value is expected). Metal rejects the shader and the
// pipeline fails to create with no other diagnostic. So buffer-addressed
// structs (records read through a pointer, like InstanceData) hold no
// pointers; per-draw pointers go in push constants. This SPIR-V walk runs on
// every platform, so the rule is caught before the macOS job.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <gtest/gtest.h>

namespace {

constexpr std::uint32_t k_magic = 0x07230203U;
constexpr std::uint32_t k_op_name = 5;
constexpr std::uint32_t k_op_type_struct = 30;
constexpr std::uint32_t k_op_type_pointer = 32;
constexpr std::uint32_t k_storage_physical = 5349; // PhysicalStorageBuffer

// "<struct name>.<member index>" for every member of a buffer-addressed struct
// that is a physical-storage pointer. Malformed modules yield {}.
std::vector<std::string> pointers_in_addressed_structs(const std::vector<std::uint32_t>& words) {
    if (words.size() < 5 || words[0] != k_magic) return {};
    std::unordered_map<std::uint32_t, std::uint32_t> psb_pointee; // pointer id -> pointee id
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> members; // struct id -> types
    std::unordered_map<std::uint32_t, std::string> names;
    for (std::size_t i = 5; i < words.size();) {
        const std::uint32_t count = words[i] >> 16U;
        const std::uint32_t op = words[i] & 0xFFFFU;
        if (count == 0 || i + count > words.size()) return {};
        if (op == k_op_type_pointer && count == 4 && words[i + 2] == k_storage_physical) {
            psb_pointee[words[i + 1]] = words[i + 3];
        } else if (op == k_op_type_struct && count >= 2) {
            members[words[i + 1]].assign(words.begin() + static_cast<std::ptrdiff_t>(i + 2),
                                         words.begin() + static_cast<std::ptrdiff_t>(i + count));
        } else if (op == k_op_name && count >= 3) {
            const auto* text = reinterpret_cast<const char*>(&words[i + 2]);
            names[words[i + 1]] = std::string(text, strnlen(text, (count - 2) * 4U));
        }
        i += count;
    }
    // Structs read through a buffer address: pointees of physical-storage
    // pointers and, transitively, structs nested in them. (A push-constant
    // block may hold pointers: its members are not array elements.)
    std::unordered_set<std::uint32_t> addressed;
    std::vector<std::uint32_t> pending;
    for (const auto& entry : psb_pointee) {
        const std::uint32_t pointee = entry.second;
        if (members.contains(pointee) && addressed.insert(pointee).second) {
            pending.push_back(pointee);
        }
    }
    while (!pending.empty()) {
        const std::uint32_t id = pending.back();
        pending.pop_back();
        for (const std::uint32_t t : members[id]) {
            if (members.contains(t) && addressed.insert(t).second) pending.push_back(t);
        }
    }
    std::vector<std::string> bad;
    for (const auto& [id, types] : members) {
        if (!addressed.contains(id)) continue;
        for (std::size_t m = 0; m < types.size(); ++m) {
            if (!psb_pointee.contains(types[m])) continue;
            const auto name = names.find(id);
            bad.push_back((name != names.end() ? name->second : "%" + std::to_string(id)) + "." +
                          std::to_string(m));
        }
    }
    return bad;
}

// Minimal module: %1 float, %2 vec4, %3 PSB ptr to %2, %4 struct{%2},
// %5 PSB ptr to %4, %6 struct{vec4, `member`}; with `addressed`, %7 PSB ptr
// to %6 (the struct is read through a buffer address, like InstanceMotion).
std::vector<std::uint32_t> module_with_member(std::uint32_t member, bool addressed = true) {
    std::vector<std::uint32_t> words{
        k_magic, 0x00010500U, 0U, 100U, 0U,
        (3U << 16U) | 22U, 1U, 32U,    // OpTypeFloat %1 32
        (4U << 16U) | 23U, 2U, 1U, 4U, // OpTypeVector %2 %1 4
        (4U << 16U) | k_op_type_pointer, 3U, k_storage_physical, 2U,
        (3U << 16U) | k_op_type_struct, 4U, 2U,
        (4U << 16U) | k_op_type_pointer, 5U, k_storage_physical, 4U,
        (4U << 16U) | k_op_type_struct, 6U, 2U, member};
    if (addressed) {
        words.insert(words.end(), {(4U << 16U) | k_op_type_pointer, 7U, k_storage_physical, 6U});
    }
    return words;
}

TEST(MslPortability, FlagsPointerMembersOfAddressedStructsOnly) {
    const std::vector<std::string> member_1{"%6.1"};
    EXPECT_EQ(pointers_in_addressed_structs(module_with_member(3)), member_1); // to a vector
    EXPECT_EQ(pointers_in_addressed_structs(module_with_member(5)), member_1); // to a struct
    EXPECT_TRUE(pointers_in_addressed_structs(module_with_member(2)).empty()); // plain vector
    // Not behind a buffer address (e.g. a push-constant block): translates.
    EXPECT_TRUE(pointers_in_addressed_structs(module_with_member(3, false)).empty());
}

TEST(MslPortability, MalformedModulesYieldNothing) {
    EXPECT_TRUE(pointers_in_addressed_structs({}).empty());
    std::vector<std::uint32_t> bad_magic = module_with_member(3);
    bad_magic[0] = 0xDEADBEEFU;
    EXPECT_TRUE(pointers_in_addressed_structs(bad_magic).empty());
    std::vector<std::uint32_t> truncated = module_with_member(3);
    truncated.pop_back();
    EXPECT_TRUE(pointers_in_addressed_structs(truncated).empty());
}

// No cooked pass or material shader keeps a pointer in a buffer-addressed
// struct.
TEST(MslPortability, NoCookedShaderNestsAPointerInABufferAddressedStruct) {
    const std::filesystem::path dir = QUAT_COOKED_SHADER_DIR;
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) GTEST_SKIP() << "no cooked shaders at " << dir;
    int checked = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(dir, ec)) {
        if (entry.path().extension() != ".spv") continue;
        std::ifstream in(entry.path(), std::ios::binary);
        std::vector<char> bytes((std::istreambuf_iterator<char>(in)), {});
        ASSERT_EQ(bytes.size() % 4, 0U) << entry.path();
        std::vector<std::uint32_t> words(bytes.size() / 4);
        std::memcpy(words.data(), bytes.data(), bytes.size());
        for (const std::string& member : pointers_in_addressed_structs(words)) {
            ADD_FAILURE() << entry.path().filename() << ": " << member
                          << " is a pointer inside a buffer-addressed struct; SPIRV-Cross "
                             "(MoltenVK) mistranslates dereferencing it (issue #103). Pass it "
                             "in push constants instead";
        }
        ++checked;
    }
    EXPECT_GT(checked, 0);
}

} // namespace
