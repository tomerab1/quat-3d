// Opaque pipelines are unchanged by alpha masking (R12.11, issue #35): the
// opaque GBuffer and shadow entry points contain no discard anywhere in
// their call graphs, so drivers keep early depth testing for them; only the
// MASK variants (and the translucent caster) can discard. Checked on the
// cooked SPIR-V the pipelines are built from.

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace {

// SPIR-V opcodes the walk needs.
constexpr std::uint32_t k_op_entry_point = 15;
constexpr std::uint32_t k_op_function = 54;
constexpr std::uint32_t k_op_function_end = 56;
constexpr std::uint32_t k_op_function_call = 57;
constexpr std::uint32_t k_op_kill = 252;
constexpr std::uint32_t k_op_terminate_invocation = 4416;
constexpr std::uint32_t k_op_demote = 5380;

struct Module {
    std::map<std::string, std::uint32_t> entry_points; // name -> function id
    std::map<std::uint32_t, std::set<std::uint32_t>> calls;
    std::set<std::uint32_t> discarding; // functions containing a discard
};

std::vector<std::uint32_t> read_words(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    const std::vector<char> bytes{std::istreambuf_iterator<char>(in),
                                  std::istreambuf_iterator<char>()};
    std::vector<std::uint32_t> words(bytes.size() / 4);
    for (std::size_t i = 0; i < words.size(); ++i) {
        words[i] = static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[4 * i])) |
                   static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[4 * i + 1])) << 8U |
                   static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[4 * i + 2])) << 16U |
                   static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[4 * i + 3])) << 24U;
    }
    return words;
}

// A SPIR-V literal string packed into words [begin, end).
std::string literal_string(const std::vector<std::uint32_t>& w, std::size_t begin,
                           std::size_t end) {
    std::string out;
    for (std::size_t k = begin; k < end; ++k) {
        for (unsigned b = 0; b < 4; ++b) {
            const char c = static_cast<char>((w[k] >> (8U * b)) & 0xFFU);
            if (c == '\0') return out;
            out.push_back(c);
        }
    }
    return out;
}

Module parse(const std::vector<std::uint32_t>& w) {
    Module m;
    std::uint32_t current = 0;
    for (std::size_t i = 5; i < w.size();) {
        const std::uint32_t count = w[i] >> 16U;
        const std::uint32_t op = w[i] & 0xFFFFU;
        if (count == 0 || i + count > w.size()) break;
        if (op == k_op_entry_point && count > 3) {
            m.entry_points[literal_string(w, i + 3, i + count)] = w[i + 2];
        } else if (op == k_op_function && count > 2) {
            current = w[i + 2];
        } else if (op == k_op_function_end) {
            current = 0;
        } else if (op == k_op_function_call && count > 3 && current != 0) {
            m.calls[current].insert(w[i + 3]);
        } else if ((op == k_op_kill || op == k_op_terminate_invocation || op == k_op_demote) &&
                   current != 0) {
            m.discarding.insert(current);
        }
        i += count;
    }
    return m;
}

// 1 when a function reachable from entry point `name` discards, 0 when none
// does, -1 when the module has no such entry point.
int can_discard(const Module& m, const std::string& name) {
    const auto entry = m.entry_points.find(name);
    if (entry == m.entry_points.end()) return -1;
    std::set<std::uint32_t> seen;
    std::vector<std::uint32_t> stack{entry->second};
    while (!stack.empty()) {
        const std::uint32_t f = stack.back();
        stack.pop_back();
        if (!seen.insert(f).second) continue;
        if (m.discarding.count(f) != 0) return 1;
        if (const auto c = m.calls.find(f); c != m.calls.end()) {
            stack.insert(stack.end(), c->second.begin(), c->second.end());
        }
    }
    return 0;
}

Module load(const char* file) {
    const std::filesystem::path path = std::filesystem::path(QUAT_COOKED_SHADER_DIR) / file;
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) return {};
    return parse(read_words(path));
}

TEST(AlphaVariantShaders, OpaqueEntryPointsNeverDiscard) {
    if (!std::filesystem::is_directory(QUAT_COOKED_SHADER_DIR)) {
        GTEST_SKIP() << "no cooked shaders at " << QUAT_COOKED_SHADER_DIR;
    }
    const Module gbuffer = load("gbuffer.spv");
    EXPECT_EQ(can_discard(gbuffer, "fragment_main"), 0);
    EXPECT_EQ(can_discard(gbuffer, "fragment_masked"), 1);
    const Module shadow = load("shadow.spv");
    EXPECT_EQ(can_discard(shadow, "fragment_main"), 0);
    const Module material = load("shadow_material.spv");
    EXPECT_EQ(can_discard(material, "vertex_main"), 0);
    EXPECT_EQ(can_discard(material, "vertex_masked"), 0);
    EXPECT_EQ(can_discard(material, "fragment_masked"), 1);
    EXPECT_EQ(can_discard(material, "fragment_translucent"), 1);
}

} // namespace
