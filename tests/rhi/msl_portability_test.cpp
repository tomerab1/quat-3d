// Cooked shaders stay translatable to Metal Shading Language (issue #103).
//
// MoltenVK converts SPIR-V to MSL with SPIRV-Cross at pipeline creation.
// SPIRV-Cross mistranslates an OpPtrAccessChain whose base is a pointer
// *value* (an OpLoad or OpCompositeExtract result) that was reached through
// an index (its expression text contains '['): it folds the new index into
// the last '[...]' of that expression instead of indexing the loaded pointer.
// `motion[instance].prev_positions[vertex]` became
// `motion[instance + vertex].prev_positions` (the wrong record, and a pointer
// where a value is expected); Metal rejects the shader and the pipeline fails
// to create with no other diagnostic. Fixed upstream by SPIRV-Cross 89a33616
// ("Do not fold OpPtrAccessChain into the chain of a loaded pointer", #2705,
// 2026-10-14), which no MoltenVK release contains yet; relax this rule once
// the macOS runners' MoltenVK has it.
//
// The rule: never index a pointer that was read through any [...]. A pointer
// read straight from a push-constant member (or through `->` members) is
// fine. This SPIR-V walk runs on every platform, so the rule is caught
// before the macOS job.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <string>
#include <unordered_map>
#include <vector>

#include <gtest/gtest.h>

namespace {

constexpr std::uint32_t k_magic = 0x07230203U;

// SPIR-V opcodes and storage classes used below.
enum Op : std::uint32_t {
    op_name = 5,
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
    op_load = 61,
    op_access_chain = 65,
    op_in_bounds_access_chain = 66,
    op_ptr_access_chain = 67,
    op_in_bounds_ptr_access_chain = 70,
    op_composite_extract = 81,
    op_copy_object = 83,
};
constexpr std::uint32_t k_uniform = 2;
constexpr std::uint32_t k_push_constant = 9;
constexpr std::uint32_t k_storage_buffer = 12;
constexpr std::uint32_t k_physical = 5349; // PhysicalStorageBuffer

struct Inst {
    std::uint32_t              op = 0;
    std::vector<std::uint32_t> operands; // everything after the opcode word
};

class Walker {
public:
    explicit Walker(const std::vector<std::uint32_t>& words) {
        if (words.size() < 5 || words[0] != k_magic) return;
        for (std::size_t i = 5; i < words.size();) {
            const std::uint32_t count = words[i] >> 16U;
            if (count == 0 || i + count > words.size()) {
                defs_.clear();
                return;
            }
            Inst inst{words[i] & 0xFFFFU,
                      {words.begin() + static_cast<std::ptrdiff_t>(i + 1),
                       words.begin() + static_cast<std::ptrdiff_t>(i + count)}};
            record(std::move(inst));
            i += count;
        }
    }

    // Names of the OpPtrAccessChains SPIRV-Cross folds wrongly.
    [[nodiscard]] std::vector<std::string> misfolded_pointer_chains() {
        std::vector<std::string> bad;
        for (const auto& [id, inst] : defs_) {
            if (inst.op != op_ptr_access_chain && inst.op != op_in_bounds_ptr_access_chain) {
                continue;
            }
            std::uint32_t base = inst.operands[2];
            for (int hops = 0; hops < 16 && op_of(base) == op_copy_object; ++hops) {
                base = defs_[base].operands[2];
            }
            const std::uint32_t base_op = op_of(base);
            if ((base_op == op_load || base_op == op_composite_extract) && indexed(base)) {
                bad.push_back(name_of(id) + " (indexes " + name_of(base) + ")");
            }
        }
        return bad;
    }

private:
    void record(Inst inst) {
        switch (inst.op) {
        case op_name: {
            if (inst.operands.size() < 2) return;
            const auto* text = reinterpret_cast<const char*>(&inst.operands[1]);
            names_[inst.operands[0]] =
                std::string(text, strnlen(text, (inst.operands.size() - 1) * 4U));
            return;
        }
        case op_type_int:
        case op_type_float:
        case op_type_vector:
        case op_type_matrix:
        case op_type_array:
        case op_type_runtime_array:
        case op_type_struct:
        case op_type_pointer:
            if (!inst.operands.empty()) defs_[inst.operands[0]] = std::move(inst);
            return;
        case op_constant:
        case op_variable:
        case op_load:
        case op_access_chain:
        case op_in_bounds_access_chain:
        case op_ptr_access_chain:
        case op_in_bounds_ptr_access_chain:
        case op_composite_extract:
        case op_copy_object: {
            // [result type, result id, first operand]; a pointer chain also
            // has its element index.
            const bool ptr_chain =
                inst.op == op_ptr_access_chain || inst.op == op_in_bounds_ptr_access_chain;
            if (inst.operands.size() < (ptr_chain ? 4U : 3U)) return;
            const std::uint32_t id = inst.operands[1];
            defs_[id] = std::move(inst);
            return;
        }
        default:
            return;
        }
    }

    [[nodiscard]] std::uint32_t op_of(std::uint32_t id) const {
        const auto it = defs_.find(id);
        return it == defs_.end() ? 0 : it->second.op;
    }

    [[nodiscard]] std::string name_of(std::uint32_t id) const {
        const auto it = names_.find(id);
        return it != names_.end() && !it->second.empty() ? it->second : "%" + std::to_string(id);
    }

    // Result type of a value instruction (0 when unknown).
    [[nodiscard]] std::uint32_t type_of(std::uint32_t id) const {
        const auto it = defs_.find(id);
        return it == defs_.end() || it->second.op == op_type_pointer ? 0
                                                                     : it->second.operands[0];
    }

    [[nodiscard]] std::uint32_t pointee(std::uint32_t pointer_type) const {
        const auto it = defs_.find(pointer_type);
        return it != defs_.end() && it->second.op == op_type_pointer &&
                       it->second.operands.size() >= 3
                   ? it->second.operands[2]
                   : 0;
    }

    [[nodiscard]] bool constant_value(std::uint32_t id, std::uint32_t& out) const {
        const auto it = defs_.find(id);
        if (it == defs_.end() || it->second.op != op_constant || it->second.operands.size() < 3) {
            return false;
        }
        out = it->second.operands[2];
        return true;
    }

    // Whether stepping from `type` through `indices` emits a '[' (arrays,
    // matrices, dynamic vector components; struct members are `.name`).
    // `literal`: the indices are literals (OpCompositeExtract), not ids.
    [[nodiscard]] bool steps_index(std::uint32_t type, const std::uint32_t* indices,
                                   std::size_t count, bool literal) const {
        for (std::size_t k = 0; k < count; ++k) {
            const auto it = defs_.find(type);
            if (it == defs_.end()) return false;
            const Inst& t = it->second;
            switch (t.op) {
            case op_type_struct: {
                std::uint32_t member = indices[k];
                if (!literal && !constant_value(indices[k], member)) return false;
                if (member + 1 >= t.operands.size()) return false;
                type = t.operands[member + 1];
                break;
            }
            case op_type_array:
            case op_type_runtime_array:
            case op_type_matrix:
                return true;
            case op_type_vector: {
                std::uint32_t component = 0;
                return !literal && !constant_value(indices[k], component);
            }
            default:
                return false;
            }
        }
        return false;
    }

    // Whether SPIRV-Cross's expression for value `id` contains a '['.
    [[nodiscard]] bool indexed(std::uint32_t id) {
        if (const auto it = memo_.find(id); it != memo_.end()) return it->second;
        memo_[id] = false; // cycles (malformed input) end here
        bool result = false;
        const auto def = defs_.find(id);
        if (def != defs_.end()) {
            const Inst& inst = def->second;
            const std::vector<std::uint32_t>& o = inst.operands;
            switch (inst.op) {
            case op_access_chain:
            case op_in_bounds_access_chain:
                result = indexed(o[2]) ||
                         steps_index(pointee(type_of(o[2])), o.data() + 3, o.size() - 3, false);
                break;
            case op_ptr_access_chain:
            case op_in_bounds_ptr_access_chain:
                result = true; // the element index itself
                break;
            case op_load:
            case op_copy_object:
                result = indexed(o[2]);
                break;
            case op_composite_extract:
                result = indexed(o[2]) ||
                         steps_index(type_of(o[2]), o.data() + 3, o.size() - 3, true);
                break;
            default:
                break;
            }
        }
        memo_[id] = result;
        return result;
    }

    std::unordered_map<std::uint32_t, Inst>        defs_;
    std::unordered_map<std::uint32_t, std::string> names_;
    std::unordered_map<std::uint32_t, bool>        memo_;
};

[[nodiscard]] std::vector<std::string> misfolded_pointer_chains(
    const std::vector<std::uint32_t>& words) {
    return Walker(words).misfolded_pointer_chains();
}

// Hand-assembled SPIR-V: just the instructions the walker reads, in the
// shapes Slang emits for each access pattern.
class Module {
public:
    // A type instruction: [result id, operands...].
    std::uint32_t type(std::uint32_t op, std::initializer_list<std::uint32_t> rest) {
        const std::uint32_t id = next_++;
        emit(op, {id}, rest);
        return id;
    }
    // A value instruction: [result type, result id, operands...].
    std::uint32_t value(std::uint32_t op, std::uint32_t type,
                        std::initializer_list<std::uint32_t> rest) {
        const std::uint32_t id = next_++;
        emit(op, {type, id}, rest);
        return id;
    }
    std::uint32_t constant(std::uint32_t v) { return value(op_constant, u32(), {v}); }
    std::uint32_t u32() {
        if (u32_ == 0) u32_ = type(op_type_int, {32, 0});
        return u32_;
    }
    std::uint32_t vec4() {
        if (vec4_ == 0) vec4_ = type(op_type_vector, {type(op_type_float, {32}), 4});
        return vec4_;
    }
    std::uint32_t ptr(std::uint32_t storage, std::uint32_t to) {
        return type(op_type_pointer, {storage, to});
    }
    std::uint32_t variable(std::uint32_t storage, std::uint32_t block) {
        return value(op_variable, ptr(storage, block), {storage});
    }
    // `pointer`[element], the pattern under test.
    std::uint32_t index(std::uint32_t pointer, std::uint32_t pointer_type) {
        return value(op_ptr_access_chain, pointer_type, {pointer, constant(2)});
    }

    [[nodiscard]] std::vector<std::uint32_t> words() const {
        std::vector<std::uint32_t> out{k_magic, 0x00010500U, 0U, next_, 0U};
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
    std::uint32_t              u32_ = 0;
    std::uint32_t              vec4_ = 0;
};

// The #103 shape: InstanceMotion read by value from motion[instance], its
// pointer member extracted, then indexed by the vertex.
TEST(MslPortability, FlagsAPointerExtractedFromAnIndexedRecord) {
    Module m;
    const std::uint32_t psb_v4 = m.ptr(k_physical, m.vec4());
    const std::uint32_t motion = m.type(op_type_struct, {m.vec4(), psb_v4});
    const std::uint32_t psb_motion = m.ptr(k_physical, motion);
    const std::uint32_t pc = m.variable(k_push_constant, m.type(op_type_struct, {psb_motion}));
    const std::uint32_t field =
        m.value(op_access_chain, m.ptr(k_push_constant, psb_motion), {pc, m.constant(0)});
    const std::uint32_t array = m.value(op_load, psb_motion, {field});
    const std::uint32_t element = m.value(op_ptr_access_chain, psb_motion, {array, m.constant(1)});
    const std::uint32_t record = m.value(op_load, motion, {element});
    const std::uint32_t pointer = m.value(op_composite_extract, psb_v4, {record, 1});
    m.index(pointer, psb_v4);
    EXPECT_EQ(misfolded_pointer_chains(m.words()).size(), 1U);
}

// Same record, the pointer loaded through a member access chain instead.
TEST(MslPortability, FlagsAPointerLoadedFromAnIndexedRecord) {
    Module m;
    const std::uint32_t psb_v4 = m.ptr(k_physical, m.vec4());
    const std::uint32_t motion = m.type(op_type_struct, {m.vec4(), psb_v4});
    const std::uint32_t psb_motion = m.ptr(k_physical, motion);
    const std::uint32_t pc = m.variable(k_push_constant, m.type(op_type_struct, {psb_motion}));
    const std::uint32_t field =
        m.value(op_access_chain, m.ptr(k_push_constant, psb_motion), {pc, m.constant(0)});
    const std::uint32_t array = m.value(op_load, psb_motion, {field});
    const std::uint32_t element = m.value(op_ptr_access_chain, psb_motion, {array, m.constant(1)});
    const std::uint32_t member = m.value(op_access_chain, m.ptr(k_physical, psb_v4),
                                         {element, m.constant(1)});
    m.index(m.value(op_load, psb_v4, {member}), psb_v4);
    EXPECT_EQ(misfolded_pointer_chains(m.words()).size(), 1U);
}

// pc.bufs[i][j]: an array of pointers in push constants.
TEST(MslPortability, FlagsAPointerFromAPushConstantArray) {
    Module m;
    const std::uint32_t psb_v4 = m.ptr(k_physical, m.vec4());
    const std::uint32_t bufs = m.type(op_type_array, {psb_v4, m.constant(4)});
    const std::uint32_t pc = m.variable(k_push_constant, m.type(op_type_struct, {bufs}));
    const std::uint32_t slot = m.value(op_access_chain, m.ptr(k_push_constant, psb_v4),
                                       {pc, m.constant(0), m.constant(1)});
    m.index(m.value(op_load, psb_v4, {slot}), psb_v4);
    EXPECT_EQ(misfolded_pointer_chains(m.words()).size(), 1U);
}

// g[i].ptr[j]: a pointer inside a StructuredBuffer element.
TEST(MslPortability, FlagsAPointerFromAStructuredBufferElement) {
    Module m;
    const std::uint32_t psb_v4 = m.ptr(k_physical, m.vec4());
    const std::uint32_t rec = m.type(op_type_struct, {psb_v4});
    const std::uint32_t runtime = m.type(op_type_runtime_array, {rec});
    const std::uint32_t g = m.variable(k_storage_buffer, m.type(op_type_struct, {runtime}));
    const std::uint32_t member = m.value(op_access_chain, m.ptr(k_storage_buffer, psb_v4),
                                         {g, m.constant(0), m.constant(1), m.constant(0)});
    m.index(m.value(op_load, psb_v4, {member}), psb_v4);
    EXPECT_EQ(misfolded_pointer_chains(m.words()).size(), 1U);
}

// cb.recs[i].ptr[j]: a pointer inside a ConstantBuffer array element.
TEST(MslPortability, FlagsAPointerFromAConstantBufferArrayElement) {
    Module m;
    const std::uint32_t psb_v4 = m.ptr(k_physical, m.vec4());
    const std::uint32_t rec = m.type(op_type_struct, {psb_v4});
    const std::uint32_t recs = m.type(op_type_array, {rec, m.constant(8)});
    const std::uint32_t cb = m.variable(k_uniform, m.type(op_type_struct, {recs}));
    const std::uint32_t member = m.value(op_access_chain, m.ptr(k_uniform, psb_v4),
                                         {cb, m.constant(0), m.constant(1), m.constant(0)});
    m.index(m.value(op_load, psb_v4, {member}), psb_v4);
    EXPECT_EQ(misfolded_pointer_chains(m.words()).size(), 1U);
}

// pc.prev_positions[j] (the #103 fix) and pc.hdr->positions[j]: no index on
// the way to the pointer, so SPIRV-Cross indexes it correctly.
TEST(MslPortability, AcceptsPointersReachedWithoutAnIndex) {
    {
        Module m;
        const std::uint32_t psb_v4 = m.ptr(k_physical, m.vec4());
        const std::uint32_t pc = m.variable(k_push_constant, m.type(op_type_struct, {psb_v4}));
        const std::uint32_t field =
            m.value(op_access_chain, m.ptr(k_push_constant, psb_v4), {pc, m.constant(0)});
        m.index(m.value(op_load, psb_v4, {field}), psb_v4);
        EXPECT_TRUE(misfolded_pointer_chains(m.words()).empty());
    }
    {
        Module m;
        const std::uint32_t psb_v4 = m.ptr(k_physical, m.vec4());
        const std::uint32_t hdr = m.type(op_type_struct, {psb_v4});
        const std::uint32_t psb_hdr = m.ptr(k_physical, hdr);
        const std::uint32_t pc = m.variable(k_push_constant, m.type(op_type_struct, {psb_hdr}));
        const std::uint32_t field =
            m.value(op_access_chain, m.ptr(k_push_constant, psb_hdr), {pc, m.constant(0)});
        const std::uint32_t header = m.value(op_load, psb_hdr, {field});
        const std::uint32_t member = m.value(op_access_chain, m.ptr(k_physical, psb_v4),
                                             {header, m.constant(0)});
        m.index(m.value(op_load, psb_v4, {member}), psb_v4);
        EXPECT_TRUE(misfolded_pointer_chains(m.words()).empty());
    }
}

TEST(MslPortability, MalformedModulesYieldNothing) {
    EXPECT_TRUE(misfolded_pointer_chains({}).empty());
    Module m;
    const std::uint32_t psb_v4 = m.ptr(k_physical, m.vec4());
    const std::uint32_t bufs = m.type(op_type_array, {psb_v4, m.constant(4)});
    const std::uint32_t pc = m.variable(k_push_constant, m.type(op_type_struct, {bufs}));
    const std::uint32_t slot = m.value(op_access_chain, m.ptr(k_push_constant, psb_v4),
                                       {pc, m.constant(0), m.constant(1)});
    m.index(m.value(op_load, psb_v4, {slot}), psb_v4);
    std::vector<std::uint32_t> bad_magic = m.words();
    bad_magic[0] = 0xDEADBEEFU;
    EXPECT_TRUE(misfolded_pointer_chains(bad_magic).empty());
    std::vector<std::uint32_t> truncated = m.words();
    truncated.pop_back();
    EXPECT_TRUE(misfolded_pointer_chains(truncated).empty());
}

// No cooked pass or material shader indexes a pointer read through [...].
TEST(MslPortability, NoCookedShaderIndexesAPointerReadThroughAnIndex) {
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
        for (const std::string& chain : misfolded_pointer_chains(words)) {
            ADD_FAILURE() << entry.path().filename() << ": " << chain
                          << " indexes a pointer that was read through [...]; MoltenVK's "
                             "SPIRV-Cross mistranslates it (issue #103). Read the pointer "
                             "from push constants instead";
        }
        ++checked;
    }
    EXPECT_GT(checked, 0);
}

} // namespace
