// Session configuration from the environment (S2, #68): loopback by default,
// all interfaces only on request, strict validation of every variable.

#include <map>
#include <string>

#include <gtest/gtest.h>

#include "engine/net/session_config.hpp"
#include "engine/net/transport.hpp"
#include "support/expect_ok.hpp"

namespace {

using engine::net::session_config_from_env;

struct FakeEnv {
    std::map<std::string, std::string> vars;
    [[nodiscard]] engine::net::EnvLookup lookup() const {
        return [this](const char* name) -> const char* {
            const auto it = vars.find(name);
            return it == vars.end() ? nullptr : it->second.c_str();
        };
    }
};

TEST(NetAddress, ParsesStrictDottedQuads) {
    EXPECT_EQ(engine::net::parse_ipv4("127.0.0.1"), 0x7F000001U);
    EXPECT_EQ(engine::net::parse_ipv4("localhost"), 0x7F000001U);
    EXPECT_EQ(engine::net::parse_ipv4("0.0.0.0"), 0U);
    EXPECT_EQ(engine::net::parse_ipv4("255.255.255.255"), 0xFFFFFFFFU);
    for (const char* bad : {"", "1.2.3", "1.2.3.4.5", "256.0.0.1", "1.2.3.4 ", " 1.2.3.4",
                            "1..2.3", "1.2.3.-4", "1.2.3.4a", "0001.2.3.4", "::", "example.com"}) {
        EXPECT_FALSE(engine::net::parse_ipv4(bad).has_value()) << "'" << bad << "'";
    }
}

TEST(NetSessionConfig, OfflineWithoutHostOrJoin) {
    const FakeEnv env;
    const auto config = session_config_from_env(env.lookup());
    ASSERT_OK(config);
    EXPECT_FALSE(config->host.has_value());
    EXPECT_FALSE(config->join.has_value());
}

TEST(NetSessionConfig, HostBindsLoopbackByDefault) {
    FakeEnv env;
    env.vars["QUAT_HOST"] = "27015";
    const auto config = session_config_from_env(env.lookup());
    ASSERT_OK(config);
    ASSERT_TRUE(config->host.has_value());
    EXPECT_EQ(config->host->server.port, 27015);
    EXPECT_EQ(config->host->server.bind, "127.0.0.1");
    EXPECT_EQ(config->host->server.max_clients, engine::net::default_max_clients);
    EXPECT_TRUE(config->host->access.join_token.empty());
}

TEST(NetSessionConfig, AllInterfacesIsAnExplicitOptIn) {
    FakeEnv env;
    env.vars["QUAT_HOST"] = "27015";
    env.vars["QUAT_NET_BIND"] = "0.0.0.0";
    env.vars["QUAT_NET_MAX_CLIENTS"] = "2";
    env.vars["QUAT_NET_TOKEN"] = "letmein";
    env.vars["QUAT_NET_OWNER_TOKEN"] = "player-one";
    const auto config = session_config_from_env(env.lookup());
    ASSERT_OK(config);
    ASSERT_TRUE(config->host.has_value());
    EXPECT_EQ(config->host->server.bind, "0.0.0.0");
    EXPECT_EQ(config->host->server.max_clients, 2U);
    EXPECT_EQ(config->host->access.join_token, "letmein");
    EXPECT_EQ(config->host->access.owner_token, "player-one");
}

TEST(NetSessionConfig, JoinCarriesTheToken) {
    FakeEnv env;
    env.vars["QUAT_JOIN"] = "192.168.1.20:27015";
    env.vars["QUAT_NET_TOKEN"] = "letmein";
    const auto config = session_config_from_env(env.lookup());
    ASSERT_OK(config);
    ASSERT_TRUE(config->join.has_value());
    EXPECT_EQ(config->join->host, "192.168.1.20");
    EXPECT_EQ(config->join->port, 27015);
    EXPECT_EQ(config->join->token, "letmein");
}

TEST(NetSessionConfig, InvalidValuesAreErrors) {
    const std::map<std::string, std::string> cases[] = {
        {{"QUAT_HOST", "0"}},
        {{"QUAT_HOST", "70000"}},
        {{"QUAT_HOST", "27015x"}},
        {{"QUAT_HOST", "27015"}, {"QUAT_NET_BIND", "all"}},
        {{"QUAT_HOST", "27015"}, {"QUAT_NET_MAX_CLIENTS", "0"}},
        {{"QUAT_HOST", "27015"}, {"QUAT_NET_MAX_CLIENTS", "-1"}},
        {{"QUAT_HOST", "27015"}, {"QUAT_NET_MAX_CLIENTS", "100000"}},
        {{"QUAT_HOST", "27015"}, {"QUAT_NET_TOKEN", std::string(300, 'x')}},
        {{"QUAT_JOIN", "127.0.0.1"}},
        {{"QUAT_JOIN", "host.example:27015"}},
        {{"QUAT_JOIN", "127.0.0.1:"}},
    };
    for (const auto& vars : cases) {
        FakeEnv env;
        env.vars = vars;
        const auto config = session_config_from_env(env.lookup());
        EXPECT_FALSE(config.has_value()) << vars.begin()->first << "=" << vars.begin()->second;
    }
}

TEST(NetSessionConfig, ErrorsDoNotEchoTokens) {
    FakeEnv env;
    env.vars["QUAT_HOST"] = "27015";
    env.vars["QUAT_NET_TOKEN"] = std::string(300, 's');
    const auto config = session_config_from_env(env.lookup());
    ASSERT_FALSE(config.has_value());
    EXPECT_EQ(config.error().message.find("sss"), std::string::npos) << config.error().message;
}

} // namespace
