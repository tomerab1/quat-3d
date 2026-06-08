// Windowless CPU smoke test for the engine_animation self-tests.
//
// The main `game` binary runs these behind QUAT_SELFTEST, but it must open an
// SDL/Vulkan window first; this harness exercises the (device-free) animation
// runtime directly, which is handy when no display is available. Returns the
// number of failed checks.

#include <cstdio>

#include "engine/animation/animator.hpp"
#include "engine/animation/clip.hpp"
#include "engine/animation/skeleton.hpp"
#include "engine/animation/state_machine.hpp"
#include "engine/core/error.hpp"

namespace {

int run(const char* name, std::expected<void, engine::core::Error> result) {
    if (result) {
        std::printf("[anim] %s OK\n", name);
        return 0;
    }
    std::printf("[anim] %s FAILED: %s\n", name, result.error().message.c_str());
    return 1;
}

} // namespace

int main() {
    using namespace engine::animation;
    int fails = 0;
    fails += run("skeleton", run_skeleton_self_test());
    fails += run("clip", run_clip_self_test());
    fails += run("animator", run_animator_self_test());
    fails += run("blend + additive", run_blend_self_test());
    fails += run("state machine", run_state_machine_self_test());
    std::printf("[anim] %s\n", fails == 0 ? "all passed" : "FAILURES");
    return fails;
}
