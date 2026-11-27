// Remote-control session tokens (issue #56): generation, the per-user token
// file location, and the user-only file the engine publishes for quat-mcp.

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include <gtest/gtest.h>

#if !defined(_WIN32)
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "engine/remote/session_token.hpp"
#include "support/expect_ok.hpp"

namespace {

namespace fs = std::filesystem;
using namespace engine::remote;

[[nodiscard]] std::string read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// A fresh scratch directory under the system temp dir, removed afterwards.
class SessionTokenFileTest : public ::testing::Test {
protected:
    fs::path root;

    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        root = fs::temp_directory_path() /
               ("quat-token-test-" + std::string(info->name()) + "-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::error_code ec;
        fs::remove_all(root, ec);
        ASSERT_TRUE(fs::create_directories(root, ec)) << ec.message();
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
};

TEST(SessionToken, Is256BitsOfHex) {
    const auto a = generate_session_token();
    const auto b = generate_session_token();
    ASSERT_OK(a);
    ASSERT_OK(b);
    EXPECT_EQ(a->size(), session_token_bytes * 2);
    EXPECT_EQ(a->find_first_not_of("0123456789abcdef"), std::string::npos) << *a;
    EXPECT_NE(*a, *b) << "two tokens from the CSPRNG were equal";
}

TEST(SessionToken, FilePathFollowsThePlatformRules) {
    TokenPathEnv env;
    env.home = "/home/ada";
    env.local_app_data = "C:\\Users\\ada\\AppData\\Local";

    env.token_file = "/custom/token";
    auto p = token_file_path(7878, env);
    ASSERT_OK(p);
    EXPECT_EQ(*p, fs::path("/custom/token")) << "QUAT_REMOTE_TOKEN_FILE wins";
    env.token_file.clear();

#if defined(__linux__)
    env.xdg_runtime_dir = "/run/user/1000";
    env.xdg_state_home = "/home/ada/.state";
    p = token_file_path(7878, env);
    ASSERT_OK(p);
    EXPECT_EQ(*p, fs::path("/run/user/1000/quat-3d/remote-7878.token"));

    env.xdg_runtime_dir = "relative/run"; // ignored: XDG paths must be absolute
    p = token_file_path(9000, env);
    ASSERT_OK(p);
    EXPECT_EQ(*p, fs::path("/home/ada/.state/quat-3d/remote-9000.token"));

    env.xdg_state_home.clear();
    p = token_file_path(9000, env);
    ASSERT_OK(p);
    EXPECT_EQ(*p, fs::path("/home/ada/.local/state/quat-3d/remote-9000.token"));

    env.home.clear();
    EXPECT_ERR(token_file_path(9000, env));
#endif
}

TEST_F(SessionTokenFileTest, PublishesAndRemovesTheToken) {
    const fs::path path = root / "quat-3d" / "remote-7878.token";
    {
        auto file = SessionTokenFile::publish(path, "abc123");
        ASSERT_OK(file);
        EXPECT_EQ(file->path(), path);
        EXPECT_EQ(read_file(path), "abc123\n");

        SessionTokenFile moved = std::move(*file); // ownership moves with it
        EXPECT_TRUE(fs::exists(path));
    }
    EXPECT_FALSE(fs::exists(path)) << "token file left behind";
    // No temporary files left either.
    EXPECT_TRUE(fs::is_empty(root / "quat-3d"));
}

#if !defined(_WIN32)
TEST_F(SessionTokenFileTest, FileAndNewDirectoryAreUserOnly) {
    const fs::path dir = root / "quat-3d";
    const fs::path path = dir / "remote-7878.token";
    const mode_t old_umask = ::umask(0); // the most permissive umask must not leak through
    auto file = SessionTokenFile::publish(path, "tok");
    ::umask(old_umask);
    ASSERT_OK(file);

    struct stat st {};
    ASSERT_EQ(::stat(path.c_str(), &st), 0);
    EXPECT_EQ(st.st_mode & 0777, 0600U) << std::oct << (st.st_mode & 0777);
    EXPECT_EQ(st.st_uid, ::geteuid());
    ASSERT_EQ(::stat(dir.c_str(), &st), 0);
    EXPECT_EQ(st.st_mode & 0777, 0700U) << std::oct << (st.st_mode & 0777);
}

// An existing world-readable file (a stale token, or one planted to be read)
// is replaced, not written through: the result is 0600 with the new token.
TEST_F(SessionTokenFileTest, ReplacesAWorldReadableFile) {
    const fs::path path = root / "remote-7878.token";
    {
        std::ofstream(path) << "old\n";
    }
    ASSERT_EQ(::chmod(path.c_str(), 0644), 0);
    auto file = SessionTokenFile::publish(path, "new");
    ASSERT_OK(file);
    EXPECT_EQ(read_file(path), "new\n");
    struct stat st {};
    ASSERT_EQ(::stat(path.c_str(), &st), 0);
    EXPECT_EQ(st.st_mode & 0777, 0600U) << std::oct << (st.st_mode & 0777);
}

// A symlink at the token path is replaced; its target is left untouched.
TEST_F(SessionTokenFileTest, DoesNotFollowASymlink) {
    const fs::path target = root / "victim";
    {
        std::ofstream(target) << "keep\n";
    }
    const fs::path path = root / "remote-7878.token";
    fs::create_symlink(target, path);
    auto file = SessionTokenFile::publish(path, "tok");
    ASSERT_OK(file);
    EXPECT_EQ(read_file(target), "keep\n");
    EXPECT_FALSE(fs::is_symlink(path));
    EXPECT_EQ(read_file(path), "tok\n");
}

TEST_F(SessionTokenFileTest, RefusesADirectoryOthersCanWrite) {
    const fs::path dir = root / "shared";
    fs::create_directories(dir);
    ASSERT_EQ(::chmod(dir.c_str(), 0777), 0); // no sticky bit
    ASSERT_ERR(SessionTokenFile::publish(dir / "remote-7878.token", "tok"));
    EXPECT_FALSE(fs::exists(dir / "remote-7878.token"));
}
#endif

} // namespace
