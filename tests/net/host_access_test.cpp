// Host access policy (S2, #68), transport-free: the hello codec, auth before
// snapshots, ownership only after auth, the auth deadline and the per-peer
// rate limits.

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <gtest/gtest.h>

#include "engine/net/host_access.hpp"
#include "engine/net/prediction.hpp"
#include "support/expect_ok.hpp"

namespace {

using engine::net::AccessConfig;
using engine::net::ClientId;
using engine::net::CloseReason;
using engine::net::HostAccess;
using Verdict = engine::net::HostAccess::Verdict;

[[nodiscard]] std::vector<std::uint8_t> hello(const std::string& token) {
    auto bytes = engine::net::encode_hello(token);
    return bytes ? *bytes : std::vector<std::uint8_t>{};
}

[[nodiscard]] std::vector<std::uint8_t> input(std::uint32_t sequence) {
    return engine::net::encode_input({sequence, glm::vec3(1.0F, 0.0F, 0.0F), 1.0F / 60.0F});
}

[[nodiscard]] AccessConfig with_token(std::string token) {
    AccessConfig config;
    config.join_token = std::move(token);
    return config;
}

// --- hello codec ------------------------------------------------------------

TEST(NetHello, RoundTrips) {
    const auto bytes = engine::net::encode_hello("s3cret");
    ASSERT_OK(bytes);
    const auto decoded = engine::net::decode_hello(*bytes);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->version, engine::net::protocol_version);
    EXPECT_EQ(decoded->token, "s3cret");
}

TEST(NetHello, RejectsMalformedPayloads) {
    std::vector<std::uint8_t> good = hello("abc");
    std::vector<std::uint8_t> truncated(good.begin(), good.end() - 1);
    std::vector<std::uint8_t> trailing = good;
    trailing.push_back(0);
    std::vector<std::uint8_t> wrong_kind = good;
    wrong_kind[0] = 2;
    // Claims a token far longer than the payload / the limit.
    std::vector<std::uint8_t> huge_length = {engine::net::hello_kind,
                                             engine::net::protocol_version, 0xFF, 0xFF};
    EXPECT_FALSE(engine::net::decode_hello(truncated).has_value());
    EXPECT_FALSE(engine::net::decode_hello(trailing).has_value());
    EXPECT_FALSE(engine::net::decode_hello(wrong_kind).has_value());
    EXPECT_FALSE(engine::net::decode_hello(huge_length).has_value());
    EXPECT_FALSE(engine::net::decode_hello({}).has_value());
    EXPECT_FALSE(engine::net::encode_hello(std::string(engine::net::max_token_bytes + 1, 'x')));
}

TEST(NetHello, ConfigValidation) {
    AccessConfig config;
    EXPECT_OK(engine::net::validate(config));
    config.auth_timeout = 0.0F;
    EXPECT_FALSE(engine::net::validate(config));
    config = {};
    config.max_messages_per_second = 0;
    EXPECT_FALSE(engine::net::validate(config));
    config = {};
    config.join_token.assign(engine::net::max_token_bytes + 1, 'x');
    EXPECT_FALSE(engine::net::validate(config));
}

// --- authentication ---------------------------------------------------------

TEST(NetHostAccess, NoSnapshotsBeforeAuthentication) {
    HostAccess access(with_token("letmein"));
    access.on_connected(1);
    EXPECT_TRUE(access.known(1));
    EXPECT_FALSE(access.authenticated(1));
    EXPECT_TRUE(access.recipients().empty()) << "connected is not authenticated";

    EXPECT_EQ(access.on_message(1, hello("letmein")).verdict, Verdict::authenticated);
    ASSERT_EQ(access.recipients().size(), 1U);
    EXPECT_EQ(access.recipients()[0], 1U);
}

TEST(NetHostAccess, WrongTokenIsDropped) {
    HostAccess access(with_token("letmein"));
    access.on_connected(1);
    const HostAccess::Decision d = access.on_message(1, hello("guess"));
    EXPECT_EQ(d.verdict, Verdict::drop);
    EXPECT_EQ(d.reason, CloseReason::auth_failed);
    EXPECT_FALSE(access.known(1)) << "a dropped peer is forgotten";
    EXPECT_TRUE(access.recipients().empty());
}

TEST(NetHostAccess, FirstMessageMustBeAHello) {
    HostAccess access; // no token: any hello authenticates, but one is required
    access.on_connected(1);
    const HostAccess::Decision d = access.on_message(1, input(1));
    EXPECT_EQ(d.verdict, Verdict::drop);
    EXPECT_EQ(d.reason, CloseReason::auth_failed);

    access.on_connected(2);
    std::vector<std::uint8_t> old_version = hello("");
    old_version[1] = engine::net::protocol_version + 1;
    EXPECT_EQ(access.on_message(2, old_version).verdict, Verdict::drop);

    access.on_connected(3);
    EXPECT_EQ(access.on_message(3, hello("anything")).verdict, Verdict::authenticated);
    EXPECT_EQ(access.on_message(3, input(1)).verdict, Verdict::deliver);
    EXPECT_EQ(access.on_message(3, hello("again")).verdict, Verdict::ignore)
        << "a repeated hello is consumed, not delivered as an input";
}

TEST(NetHostAccess, UnknownPeersAreIgnored) {
    HostAccess access;
    EXPECT_EQ(access.on_message(9, hello("")).verdict, Verdict::ignore);
    EXPECT_FALSE(access.known(9));
}

// --- ownership --------------------------------------------------------------

TEST(NetHostAccess, OwnershipGoesToTheFirstAuthenticatedPeer) {
    HostAccess access(with_token("letmein"));
    access.on_connected(1); // connects first ...
    access.on_connected(2);
    EXPECT_EQ(access.owner(), engine::net::invalid_client) << "nobody owns before auth";

    // ... but peer 2 authenticates first; peer 1's input does not claim it.
    EXPECT_EQ(access.on_message(2, hello("letmein")).verdict, Verdict::authenticated);
    EXPECT_EQ(access.owner(), 2U);
    EXPECT_EQ(access.on_message(1, input(1)).verdict, Verdict::drop);
    EXPECT_EQ(access.owner(), 2U);
}

TEST(NetHostAccess, OwnershipPassesToTheNextAuthenticatedPeer) {
    HostAccess access;
    for (ClientId c : {1U, 2U, 3U}) access.on_connected(c);
    EXPECT_EQ(access.on_message(3, hello("")).verdict, Verdict::authenticated);
    EXPECT_EQ(access.on_message(1, hello("")).verdict, Verdict::authenticated);
    EXPECT_EQ(access.owner(), 3U);

    access.on_disconnected(3);
    EXPECT_EQ(access.owner(), 1U) << "longest-authenticated remaining peer";
    access.on_disconnected(1);
    EXPECT_EQ(access.owner(), engine::net::invalid_client)
        << "peer 2 never authenticated, so it cannot inherit the character";
}

TEST(NetHostAccess, OwnerTokenDesignatesTheOwner) {
    AccessConfig config;
    config.join_token = "guest";
    config.owner_token = "player-one";
    HostAccess access(config);
    access.on_connected(1);
    access.on_connected(2);
    EXPECT_EQ(access.on_message(1, hello("guest")).verdict, Verdict::authenticated);
    EXPECT_EQ(access.owner(), engine::net::invalid_client)
        << "a guest is authenticated (snapshots) but may not own";
    EXPECT_EQ(access.recipients().size(), 1U);

    EXPECT_EQ(access.on_message(2, hello("player-one")).verdict, Verdict::authenticated);
    EXPECT_EQ(access.owner(), 2U);
    access.on_disconnected(2);
    EXPECT_EQ(access.owner(), engine::net::invalid_client) << "guests never inherit it";
}

// --- auth deadline ----------------------------------------------------------

TEST(NetHostAccess, SilentPeersAreDroppedAtTheAuthDeadline) {
    AccessConfig config;
    config.auth_timeout = 3.0F;
    HostAccess access(config);
    access.on_connected(1);
    access.on_connected(2);
    EXPECT_EQ(access.on_message(2, hello("")).verdict, Verdict::authenticated);

    EXPECT_TRUE(access.tick(1.0F).empty());
    EXPECT_TRUE(access.tick(1.5F).empty());
    const std::vector<ClientId> late = access.tick(0.6F);
    ASSERT_EQ(late.size(), 1U);
    EXPECT_EQ(late[0], 1U);
    EXPECT_FALSE(access.known(1));
    EXPECT_TRUE(access.known(2)) << "authenticated peers have no deadline";
    EXPECT_TRUE(access.tick(100.0F).empty());
}

TEST(NetHostAccess, BadTimeStepsDoNotAdvanceTheDeadline) {
    HostAccess access;
    access.on_connected(1);
    EXPECT_TRUE(access.tick(-50.0F).empty());
    EXPECT_TRUE(access.tick(std::numeric_limits<float>::quiet_NaN()).empty());
    EXPECT_TRUE(access.known(1));
}

// --- rate limits ------------------------------------------------------------

TEST(NetHostAccess, MessageFloodIsDropped) {
    AccessConfig config;
    config.max_messages_per_second = 100;
    HostAccess access(config);
    access.on_connected(1);
    ASSERT_EQ(access.on_message(1, hello("")).verdict, Verdict::authenticated);

    // The burst allowance (rate_burst_seconds of budget) is spent, then the
    // next message costs the peer its connection.
    const auto burst = static_cast<std::uint32_t>(100.0F * engine::net::rate_burst_seconds);
    std::uint32_t seq = 0;
    for (std::uint32_t i = 1; i < burst; ++i) {
        ASSERT_EQ(access.on_message(1, input(++seq)).verdict, Verdict::deliver) << i;
    }
    const HostAccess::Decision d = access.on_message(1, input(++seq));
    EXPECT_EQ(d.verdict, Verdict::drop);
    EXPECT_EQ(d.reason, CloseReason::rate_limited);
    EXPECT_FALSE(access.known(1));
    EXPECT_TRUE(access.recipients().empty());
}

TEST(NetHostAccess, SteadyTrafficWithinTheRateIsKept) {
    AccessConfig config;
    config.max_messages_per_second = 120;
    HostAccess access(config);
    access.on_connected(1);
    ASSERT_EQ(access.on_message(1, hello("")).verdict, Verdict::authenticated);
    // 60 s at 120 msg/s: budget refills each frame, never exhausted.
    std::uint32_t seq = 0;
    for (int frame = 0; frame < 3600; ++frame) {
        ASSERT_TRUE(access.tick(1.0F / 60.0F).empty());
        for (int i = 0; i < 2; ++i) {
            ASSERT_EQ(access.on_message(1, input(++seq)).verdict, Verdict::deliver)
                << "frame " << frame;
        }
    }
}

TEST(NetHostAccess, ByteFloodIsDropped) {
    AccessConfig config;
    config.max_bytes_per_second = 1024;
    HostAccess access(config);
    access.on_connected(1);
    ASSERT_EQ(access.on_message(1, hello("")).verdict, Verdict::authenticated);
    const std::vector<std::uint8_t> big(4096, 0); // above the 2 s burst of bytes
    const HostAccess::Decision d = access.on_message(1, big);
    EXPECT_EQ(d.verdict, Verdict::drop);
    EXPECT_EQ(d.reason, CloseReason::rate_limited);
}

TEST(NetHostAccess, UnauthenticatedFloodIsRateLimitedToo) {
    AccessConfig config;
    config.max_bytes_per_second = 16;
    HostAccess access(config);
    access.on_connected(1);
    // An oversized "hello" is refused by the byte budget before it is parsed.
    const std::vector<std::uint8_t> big(engine::net::max_token_bytes * 4, 0);
    EXPECT_EQ(access.on_message(1, big).reason, CloseReason::rate_limited);
}

} // namespace
