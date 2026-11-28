#pragma once

// JsonFields — type-checked reads of a JSON object's fields (Phase 15).
//
// The engine builds without exceptions, and nlohmann::json reports type errors
// by throwing — which, with -fno-exceptions, aborts the process. Any JSON that
// did not come from the engine itself (a scene file on disk, a remote-control
// request) must therefore be read through checks, never `get<T>()`/`value()`
// directly. JsonFields does that: each read verifies the field's type and
// range, records the first failure with a readable path ("point_light.radius:
// expected a number"), and otherwise leaves the destination untouched, so
// absent fields keep their defaults (merge semantics for partial updates).
//
//     core::JsonFields f(params, "point_light");
//     f.number("radius", light.radius, 0.0F).vec3("color", light.color);
//     if (auto ok = f.finish(); !ok) return std::unexpected(ok.error());
//
// finish() also rejects fields that were never read when `strict` is set —
// remote clients (language models in particular) get a precise error for a
// misspelt field instead of a silent no-op.

#include <cmath>
#include <cstdint>
#include <expected>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include "engine/core/error.hpp"

namespace engine::core {

// One accepted spelling of an enum value. Readers accept the name, or (for
// legacy files) the integer value.
template <typename E>
struct EnumName {
    std::string_view name;
    E                value;
};

class JsonFields {
public:
    using Json = nlohmann::json;

    // `context` prefixes error messages. `strict` makes finish() reject
    // unread keys. A non-object `object` is itself an error.
    JsonFields(const Json& object, std::string context, bool strict = true)
        : object_(object), context_(std::move(context)), strict_(strict) {
        if (!object_.is_object()) fail_root("expected an object");
    }

    [[nodiscard]] bool has(std::string_view key) const {
        return object_.is_object() && object_.contains(key);
    }

    // Raw access for fields a caller decodes itself (nested objects); marks
    // the key consumed. Null when absent.
    [[nodiscard]] const Json* raw(std::string_view key) {
        if (!has(key)) return nullptr;
        consume(key);
        return &object_.find(key).value();
    }

    JsonFields& boolean(std::string_view key, bool& out) {
        if (const Json* v = take(key)) {
            if (v->is_boolean()) {
                out = v->get<bool>();
            } else {
                fail(key, "expected true or false");
            }
        }
        return *this;
    }

    // Finite number within [lo, hi].
    JsonFields& number(std::string_view key, float& out,
                       float lo = std::numeric_limits<float>::lowest(),
                       float hi = std::numeric_limits<float>::max()) {
        if (const Json* v = take(key)) {
            float f = 0.0F;
            if (read_float(*v, f) && f >= lo && f <= hi) {
                out = f;
            } else {
                fail(key, range_message(lo, hi));
            }
        }
        return *this;
    }

    // Finite double within [lo, hi], for values a float cannot carry
    // exactly (calendar time, geographic coordinates).
    JsonFields& number(std::string_view key, double& out,
                       double lo = std::numeric_limits<double>::lowest(),
                       double hi = std::numeric_limits<double>::max()) {
        if (const Json* v = take(key)) {
            if (v->is_number() && std::isfinite(v->get<double>()) && v->get<double>() >= lo &&
                v->get<double>() <= hi) {
                out = v->get<double>();
            } else {
                fail(key, range_message(static_cast<float>(lo), static_cast<float>(hi)));
            }
        }
        return *this;
    }

    // Integer within [lo, hi] (a float with no fractional part is accepted).
    template <typename I>
    JsonFields& integer(std::string_view key, I& out,
                        std::int64_t lo = std::numeric_limits<std::int32_t>::min(),
                        std::int64_t hi = std::numeric_limits<std::int32_t>::max()) {
        if (const Json* v = take(key)) {
            std::int64_t i = 0;
            if (read_int(*v, i) && i >= lo && i <= hi) {
                out = static_cast<I>(i);
            } else {
                fail(key, "expected an integer in [" + std::to_string(lo) + ", " +
                              std::to_string(hi) + "]");
            }
        }
        return *this;
    }

    JsonFields& string(std::string_view key, std::string& out, std::size_t max_len = 1U << 20) {
        if (const Json* v = take(key)) {
            if (v->is_string() && v->get_ref<const std::string&>().size() <= max_len) {
                out = v->get<std::string>();
            } else {
                fail(key, "expected a string of at most " + std::to_string(max_len) + " bytes");
            }
        }
        return *this;
    }

    JsonFields& vec3(std::string_view key, glm::vec3& out) {
        if (const Json* v = take(key)) {
            if (!read_vec3(*v, out)) fail(key, "expected an array of 3 finite numbers");
        }
        return *this;
    }

    // RGB colour: [r, g, b] in [0, max].
    JsonFields& color3(std::string_view key, glm::vec3& out, float max = 1.0e4F) {
        if (const Json* v = take(key)) {
            glm::vec3 c{};
            if (read_vec3(*v, c) && glm::all(glm::greaterThanEqual(c, glm::vec3(0.0F))) &&
                glm::all(glm::lessThanEqual(c, glm::vec3(max)))) {
                out = c;
            } else {
                fail(key, "expected [r, g, b] with components >= 0");
            }
        }
        return *this;
    }

    // RGBA colour: [r, g, b] (alpha kept) or [r, g, b, a], components in [0, 1].
    JsonFields& color4(std::string_view key, glm::vec4& out) {
        if (const Json* v = take(key)) {
            bool ok = v->is_array() && (v->size() == 3 || v->size() == 4);
            glm::vec4 c = out;
            for (std::size_t i = 0; ok && i < v->size(); ++i) {
                float f = 0.0F;
                ok = read_float((*v)[i], f) && f >= 0.0F && f <= 1.0F;
                c[static_cast<glm::length_t>(i)] = f;
            }
            if (ok) {
                out = c;
            } else {
                fail(key, "expected [r, g, b] or [r, g, b, a] with components in [0, 1]");
            }
        }
        return *this;
    }

    template <typename E>
    JsonFields& enumeration(std::string_view key, E& out, std::span<const EnumName<E>> names) {
        if (const Json* v = take(key)) {
            bool matched = false;
            if (v->is_string()) {
                const auto& s = v->get_ref<const std::string&>();
                for (const auto& n : names) {
                    if (n.name == s) {
                        out = n.value;
                        matched = true;
                    }
                }
            } else if (std::int64_t i = 0; read_int(*v, i)) { // legacy numeric encoding
                for (const auto& n : names) {
                    if (static_cast<std::int64_t>(n.value) == i) {
                        out = n.value;
                        matched = true;
                    }
                }
            }
            if (!matched) {
                std::string msg = "expected one of";
                for (std::size_t i = 0; i < names.size(); ++i) {
                    msg += (i == 0 ? " \"" : ", \"") + std::string(names[i].name) + "\"";
                }
                fail(key, msg);
            }
        }
        return *this;
    }

    // Convenience for a fixed table of names (deduces E through the array).
    template <typename E, std::size_t N>
    JsonFields& enumeration(std::string_view key, E& out, const EnumName<E> (&names)[N]) {
        return enumeration(key, out, std::span<const EnumName<E>>(names, N));
    }

    // Array of [x, y, z] points (at most `max_count`).
    JsonFields& vec3_list(std::string_view key, std::vector<glm::vec3>& out,
                          std::size_t max_count = 4096) {
        if (const Json* v = take(key)) {
            bool ok = v->is_array() && v->size() <= max_count;
            std::vector<glm::vec3> pts;
            for (std::size_t i = 0; ok && i < v->size(); ++i) {
                glm::vec3 p{};
                ok = read_vec3((*v)[i], p);
                pts.push_back(p);
            }
            if (ok) {
                out = std::move(pts);
            } else {
                fail(key, "expected an array of [x, y, z] points (at most " +
                              std::to_string(max_count) + ")");
            }
        }
        return *this;
    }

    // Array of [x, y] (double) points (at most `max_count`), e.g. polygons.
    JsonFields& dvec2_list(std::string_view key, std::vector<glm::dvec2>& out,
                           std::size_t max_count = 4096) {
        if (const Json* v = take(key)) {
            if (!read_dvec2_list(*v, out, max_count)) {
                fail(key, "expected an array of [x, y] points (at most " + std::to_string(max_count) + ")");
            }
        }
        return *this;
    }

    // Array of [x, y, z] (double) points (at most `max_count`), e.g. paths.
    JsonFields& dvec3_list(std::string_view key, std::vector<glm::dvec3>& out,
                           std::size_t max_count = 4096) {
        if (const Json* v = take(key)) {
            bool ok = v->is_array() && v->size() <= max_count;
            std::vector<glm::dvec3> pts;
            for (std::size_t i = 0; ok && i < v->size(); ++i) {
                const Json& p = (*v)[i];
                glm::dvec3 q{};
                ok = p.is_array() && p.size() == 3 && read_double(p[0], q.x) && read_double(p[1], q.y) &&
                     read_double(p[2], q.z);
                pts.push_back(q);
            }
            if (ok) {
                out = std::move(pts);
            } else {
                fail(key, "expected an array of [x, y, z] points (at most " + std::to_string(max_count) + ")");
            }
        }
        return *this;
    }

    // Array of finite numbers (at most `max_count`).
    JsonFields& number_list(std::string_view key, std::vector<double>& out, std::size_t max_count = 4096) {
        if (const Json* v = take(key)) {
            bool ok = v->is_array() && v->size() <= max_count;
            std::vector<double> values;
            for (std::size_t i = 0; ok && i < v->size(); ++i) {
                double d = 0.0;
                ok = read_double((*v)[i], d);
                values.push_back(d);
            }
            if (ok) {
                out = std::move(values);
            } else {
                fail(key, "expected an array of at most " + std::to_string(max_count) + " finite numbers");
            }
        }
        return *this;
    }

    // Records an externally detected problem with `key` (first error wins).
    void fail(std::string_view key, const std::string& what) {
        if (error_.empty()) error_ = path(key) + ": " + what;
    }

    [[nodiscard]] bool ok() const { return error_.empty(); }
    // Lenient decoders (scene files) skip values that no longer resolve.
    [[nodiscard]] bool strict() const { return strict_; }

    // First recorded error, or (strict) the first unread key.
    [[nodiscard]] std::expected<void, Error> finish() const {
        if (!error_.empty()) return std::unexpected(Error{error_});
        if (strict_ && object_.is_object()) {
            for (const auto& [key, value] : object_.items()) {
                bool seen = false;
                for (const std::string& c : consumed_) seen = seen || c == key;
                if (!seen) {
                    std::string known;
                    for (const std::string& c : known_) known += (known.empty() ? "" : ", ") + c;
                    return std::unexpected(Error{path(key) + ": unknown field" +
                                                 (known.empty() ? "" : " (known: " + known + ")")});
                }
            }
        }
        return {};
    }

    // Declares keys the caller accepts but reads through raw()/elsewhere, so
    // they appear in the "known" list of unknown-field errors.
    JsonFields& known(std::initializer_list<std::string_view> keys) {
        for (std::string_view k : keys) note_known(k);
        return *this;
    }

    // Safe scalar conversions, usable on loose values (array elements etc.).
    [[nodiscard]] static bool read_float(const Json& v, float& out) {
        if (!v.is_number()) return false;
        const double d = v.get<double>();
        if (!std::isfinite(d) || std::abs(d) > static_cast<double>(std::numeric_limits<float>::max())) {
            return false;
        }
        out = static_cast<float>(d);
        return true;
    }
    [[nodiscard]] static bool read_double(const Json& v, double& out) {
        if (!v.is_number()) return false;
        const double d = v.get<double>();
        if (!std::isfinite(d)) return false;
        out = d;
        return true;
    }
    // [[x, y], ...] with at most `max_count` points; `out` is untouched on failure.
    [[nodiscard]] static bool read_dvec2_list(const Json& v, std::vector<glm::dvec2>& out, std::size_t max_count) {
        if (!v.is_array() || v.size() > max_count) return false;
        std::vector<glm::dvec2> pts;
        pts.reserve(v.size());
        for (std::size_t i = 0; i < v.size(); ++i) {
            const Json& p = v[i];
            glm::dvec2 q{};
            if (!p.is_array() || p.size() != 2 || !read_double(p[0], q.x) || !read_double(p[1], q.y)) return false;
            pts.push_back(q);
        }
        out = std::move(pts);
        return true;
    }
    [[nodiscard]] static bool read_bool(const Json& v, bool& out) {
        if (!v.is_boolean()) return false;
        out = v.get<bool>();
        return true;
    }
    [[nodiscard]] static bool read_int(const Json& v, std::int64_t& out) {
        if (v.is_number_integer()) {
            if (v.is_number_unsigned() &&
                v.get<std::uint64_t>() > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
                return false;
            }
            out = v.get<std::int64_t>();
            return true;
        }
        if (v.is_number_float()) {
            const double d = v.get<double>();
            if (!std::isfinite(d) || std::floor(d) != d || std::abs(d) > 9.0e15) return false;
            out = static_cast<std::int64_t>(d);
            return true;
        }
        return false;
    }
    [[nodiscard]] static bool read_string(const Json& v, std::string& out) {
        if (!v.is_string()) return false;
        out = v.get_ref<const std::string&>();
        return true;
    }
    [[nodiscard]] static bool read_vec3(const Json& v, glm::vec3& out) {
        if (!v.is_array() || v.size() != 3) return false;
        glm::vec3 r{};
        for (std::size_t i = 0; i < 3; ++i) {
            if (!read_float(v[i], r[static_cast<glm::length_t>(i)])) return false;
        }
        out = r;
        return true;
    }

private:
    [[nodiscard]] const Json* take(std::string_view key) {
        note_known(key);
        if (!has(key)) return nullptr;
        consume(key);
        const Json& v = object_.find(key).value();
        return v.is_null() ? nullptr : &v; // explicit null == absent
    }
    void consume(std::string_view key) {
        for (const std::string& c : consumed_) {
            if (c == key) return;
        }
        consumed_.emplace_back(key);
    }
    void note_known(std::string_view key) {
        for (const std::string& k : known_) {
            if (k == key) return;
        }
        known_.emplace_back(key);
    }
    void fail_root(const std::string& what) {
        if (error_.empty()) error_ = context_ + ": " + what;
    }
    [[nodiscard]] std::string path(std::string_view key) const {
        return context_.empty() ? std::string(key) : context_ + "." + std::string(key);
    }
    [[nodiscard]] static std::string range_message(float lo, float hi) {
        const bool has_lo = lo > std::numeric_limits<float>::lowest();
        const bool has_hi = hi < std::numeric_limits<float>::max();
        if (has_lo && has_hi) {
            return "expected a number in [" + trim(lo) + ", " + trim(hi) + "]";
        }
        if (has_lo) return "expected a number >= " + trim(lo);
        if (has_hi) return "expected a number <= " + trim(hi);
        return "expected a finite number";
    }
    [[nodiscard]] static std::string trim(float f) {
        std::string s = std::to_string(f);
        while (s.size() > 1 && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
        return s;
    }

    const Json&              object_;
    std::string              context_;
    bool                     strict_ = true;
    std::string              error_;
    std::vector<std::string> consumed_;
    std::vector<std::string> known_;
};

} // namespace engine::core
