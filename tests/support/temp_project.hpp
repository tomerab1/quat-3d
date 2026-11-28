#pragma once

// A scratch directory for tests that build files on disk (game projects,
// packages), removed on destruction. write() creates parent directories.

#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <system_error>

namespace quat::test {

class TempProject {
public:
    explicit TempProject(const std::string& prefix = "quat_test") {
        std::random_device rd;
        path_ = std::filesystem::temp_directory_path() /
                (prefix + "_" + std::to_string(rd()) + std::to_string(rd()));
        std::filesystem::create_directories(path_);
    }
    ~TempProject() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    TempProject(const TempProject&) = delete;
    TempProject& operator=(const TempProject&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

    void write(const std::string& relative, const std::string& text) const {
        const std::filesystem::path p = path_ / relative;
        std::filesystem::create_directories(p.parent_path());
        std::ofstream(p, std::ios::binary) << text;
    }

    [[nodiscard]] std::string read(const std::string& relative) const {
        std::ifstream in(path_ / relative, std::ios::binary);
        return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    }

    [[nodiscard]] bool exists(const std::string& relative) const {
        std::error_code ec;
        return std::filesystem::exists(path_ / relative, ec);
    }

private:
    std::filesystem::path path_;
};

// A minimal valid game.json for a project whose start scene is `scene`.
[[nodiscard]] inline std::string game_json(const std::string& scene = "assets/scenes/main.json",
                                           const std::string& extra = "") {
    return std::string(R"({"format": 1, "name": "Test Game", "id": "dev.quat3d.test-game", )") +
           R"("version": "1.2.3", "start_scene": ")" + scene + "\"" +
           (extra.empty() ? "" : ", " + extra) + "}";
}

} // namespace quat::test
