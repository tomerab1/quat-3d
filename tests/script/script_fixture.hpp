#pragma once

// Shared fixture for the script runtime tests: a scratch scripts directory,
// a Scene, a ScriptRuntime over them, and the script lines of the engine
// log ring (print() / log.* / errors).

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include <entt/entity/registry.hpp>
#include <gtest/gtest.h>

#include "engine/core/log.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/scene.hpp"
#include "engine/script/script_runtime.hpp"
#include "support/expect_ok.hpp"
#include "support/temp_dir.hpp"

namespace quat::test {

class ScriptTest : public ::testing::Test {
protected:
    TempDir                                     dir{"scripts"};
    engine::scene::Scene                        scene;
    std::optional<engine::script::ScriptRuntime> runtime;
    std::uint64_t                               log_mark = engine::core::log_ring().last_seq();

    void write(const std::string& relative, const std::string& source) {
        const std::filesystem::path file = dir.path() / relative;
        std::filesystem::create_directories(file.parent_path());
        std::ofstream(file, std::ios::binary | std::ios::trunc) << source;
    }

    engine::script::ScriptRuntime& start(engine::script::ScriptLimits limits = {}) {
        runtime.emplace(engine::script::ScriptRuntime::Options{dir.path(), limits, false});
        EXPECT_OK(runtime->start(scene));
        return *runtime;
    }

    [[nodiscard]] static engine::scene::Script script(const std::string& path) {
        engine::scene::Script s;
        s.path = path;
        return s;
    }

    entt::entity scripted(const std::string& name, const std::string& path) {
        const entt::entity e = scene.create_entity(name);
        scene.registry().emplace<engine::scene::Script>(e, script(path));
        return e;
    }

    // Runs `frames` updates of 1/60 s.
    void frames(int n = 1) {
        for (int i = 0; i < n; ++i) runtime->update(1.0F / 60.0F);
    }

    // Messages of the "script" log lines since the fixture was created (or
    // the last call), oldest first.
    std::vector<std::string> script_log() {
        engine::core::LogQuery q;
        q.since_seq = log_mark;
        q.category = "script";
        q.limit = 1000;
        const engine::core::LogQueryResult r = engine::core::log_ring().query(q);
        log_mark = r.cursor;
        std::vector<std::string> out;
        for (const engine::core::LogLine& l : r.lines) out.push_back(l.message);
        return out;
    }

    // True when some script log line contains `text`.
    [[nodiscard]] static bool has_line(const std::vector<std::string>& lines,
                                       const std::string& text) {
        for (const std::string& l : lines) {
            if (l.find(text) != std::string::npos) return true;
        }
        return false;
    }

    [[nodiscard]] const engine::script::ScriptStatus* status(const std::string& path) const {
        for (const auto& s : runtime->statuses()) {
            if (s.path == path) return &s;
        }
        return nullptr;
    }
};

} // namespace quat::test
