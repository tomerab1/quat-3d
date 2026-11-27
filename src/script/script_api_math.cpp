// vec3 / quat for scripts — see docs/scripting.md ("Math").

#include <cmath>
#include <string>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/euler_angles.hpp>

#include "lua_vm.hpp"
#include "script_api.hpp"

namespace engine::script::detail {

namespace {

using glm::quat;
using glm::vec3;

[[nodiscard]] sol::automagic_enrollments no_automagic() {
    sol::automagic_enrollments e;
    e.default_constructor = false;
    e.pairs_operator = false;
    e.to_string_operator = false;
    e.call_operator = false;
    e.less_than_operator = false;
    e.less_than_or_equal_to_operator = false;
    e.length_operator = false;
    e.equal_to_operator = false;
    return e;
}

[[nodiscard]] std::string format_number(float v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.4g", static_cast<double>(v));
    return buf;
}

[[nodiscard]] vec3 safe_normalize(const vec3& v) {
    const float len = glm::length(v);
    return len > 1e-12F ? v / len : vec3(0.0F);
}

// Euler degrees in the codec's convention: x = pitch, y = yaw, z = roll,
// applied yaw (Y), then pitch (X), then roll (Z).
[[nodiscard]] quat from_euler_deg(const vec3& deg) {
    const vec3 r = glm::radians(deg);
    return glm::quat_cast(glm::eulerAngleYXZ(r.y, r.x, r.z));
}

[[nodiscard]] vec3 to_euler_deg(const quat& q) {
    float yaw = 0.0F;
    float pitch = 0.0F;
    float roll = 0.0F;
    glm::extractEulerAngleYXZ(glm::mat4_cast(q), yaw, pitch, roll);
    return glm::degrees(vec3(pitch, yaw, roll));
}

// Rotation whose -Z (forward) points along `forward`, +Y towards `up`.
[[nodiscard]] quat look_rotation(const vec3& forward, const vec3& up) {
    const vec3 f = safe_normalize(forward);
    if (glm::length(f) == 0.0F) return quat(1.0F, 0.0F, 0.0F, 0.0F);
    vec3 u = safe_normalize(up);
    if (glm::length(u) == 0.0F || std::abs(glm::dot(f, u)) > 0.9999F) {
        u = std::abs(f.y) < 0.9999F ? vec3(0.0F, 1.0F, 0.0F) : vec3(0.0F, 0.0F, 1.0F);
    }
    return glm::quatLookAtRH(f, u);
}

void register_vec3(sol::state_view lua) {
    auto t = lua.new_usertype<vec3>("vec3", no_automagic());
    t.set(sol::call_constructor,
          sol::factories([] { return vec3(0.0F); }, [](float s) { return vec3(s); },
                         [](float x, float y, float z) { return vec3(x, y, z); }));
    t.set("x", sol::property([](const vec3& v) { return v.x; }, [](vec3& v, float f) { v.x = f; }));
    t.set("y", sol::property([](const vec3& v) { return v.y; }, [](vec3& v, float f) { v.y = f; }));
    t.set("z", sol::property([](const vec3& v) { return v.z; }, [](vec3& v, float f) { v.z = f; }));
    t.set(sol::meta_function::addition, [](const vec3& a, const vec3& b) { return a + b; });
    t.set(sol::meta_function::subtraction, [](const vec3& a, const vec3& b) { return a - b; });
    t.set(sol::meta_function::multiplication,
          sol::overload([](const vec3& a, const vec3& b) { return a * b; },
                        [](const vec3& a, float s) { return a * s; },
                        [](float s, const vec3& a) { return s * a; }));
    t.set(sol::meta_function::division,
          sol::overload([](const vec3& a, const vec3& b) { return a / b; },
                        [](const vec3& a, float s) { return a / s; }));
    t.set(sol::meta_function::unary_minus, [](const vec3& a) { return -a; });
    t.set(sol::meta_function::equal_to, [](const vec3& a, const vec3& b) { return a == b; });
    t.set(sol::meta_function::to_string, [](const vec3& v) {
        return "vec3(" + format_number(v.x) + ", " + format_number(v.y) + ", " +
               format_number(v.z) + ")";
    });
    t.set("length", [](const vec3& v) { return glm::length(v); });
    t.set("length_squared", [](const vec3& v) { return glm::dot(v, v); });
    t.set("normalized", [](const vec3& v) { return safe_normalize(v); });
    t.set("dot", [](const vec3& a, const vec3& b) { return glm::dot(a, b); });
    t.set("cross", [](const vec3& a, const vec3& b) { return glm::cross(a, b); });
    t.set("distance", [](const vec3& a, const vec3& b) { return glm::distance(a, b); });
    t.set("lerp", [](const vec3& a, const vec3& b, float k) { return glm::mix(a, b, k); });
    t.set("copy", [](const vec3& v) { return v; });
}

void register_quat(sol::state_view lua) {
    auto t = lua.new_usertype<quat>("quat", no_automagic());
    t.set(sol::call_constructor,
          sol::factories([] { return quat(1.0F, 0.0F, 0.0F, 0.0F); },
                         [](float x, float y, float z, float w) { return quat(w, x, y, z); }));
    t.set("x", sol::property([](const quat& q) { return q.x; }, [](quat& q, float f) { q.x = f; }));
    t.set("y", sol::property([](const quat& q) { return q.y; }, [](quat& q, float f) { q.y = f; }));
    t.set("z", sol::property([](const quat& q) { return q.z; }, [](quat& q, float f) { q.z = f; }));
    t.set("w", sol::property([](const quat& q) { return q.w; }, [](quat& q, float f) { q.w = f; }));
    t.set("euler", [](const vec3& deg) { return from_euler_deg(deg); });
    t.set("angle_axis", [](float deg, const vec3& axis) {
        const vec3 a = safe_normalize(axis);
        return glm::length(a) > 0.0F ? glm::angleAxis(glm::radians(deg), a)
                                     : quat(1.0F, 0.0F, 0.0F, 0.0F);
    });
    t.set("look_rotation", [](const vec3& forward, sol::optional<vec3> up) {
        return look_rotation(forward, up.value_or(vec3(0.0F, 1.0F, 0.0F)));
    });
    t.set(sol::meta_function::multiplication,
          sol::overload([](const quat& a, const quat& b) { return a * b; },
                        [](const quat& q, const vec3& v) { return q * v; }));
    t.set(sol::meta_function::equal_to, [](const quat& a, const quat& b) { return a == b; });
    t.set(sol::meta_function::to_string, [](const quat& q) {
        return "quat(" + format_number(q.x) + ", " + format_number(q.y) + ", " +
               format_number(q.z) + ", " + format_number(q.w) + ")";
    });
    t.set("to_euler", [](const quat& q) { return to_euler_deg(q); });
    t.set("inverse", [](const quat& q) { return glm::inverse(q); });
    t.set("normalized", [](const quat& q) { return glm::normalize(q); });
    t.set("slerp", [](const quat& a, const quat& b, float k) { return glm::slerp(a, b, k); });
    t.set("rotate", [](const quat& q, const vec3& v) { return q * v; });
}

} // namespace

void register_math(LuaVm& vm) {
    sol::state_view lua = vm.lua();
    register_vec3(lua);
    register_quat(lua);
    vm.expose("vec3", lua["vec3"]);
    vm.expose("quat", lua["quat"]);
}

} // namespace engine::script::detail
