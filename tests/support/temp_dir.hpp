#pragma once

// A uniquely named scratch directory under the system temp directory, removed
// (recursively) when the object goes out of scope.

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>
#include <system_error>

namespace quat::test {

class TempDir {
public:
    explicit TempDir(const std::string& prefix) {
        static std::atomic<unsigned> counter{0};
        std::error_code ec;
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path(ec) /
                ("quat-" + prefix + "-" + std::to_string(stamp) + "-" +
                 std::to_string(counter.fetch_add(1)));
        std::filesystem::create_directories(path_, ec);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

} // namespace quat::test
