#include <gtest/gtest.h>
#include "config.hpp"

TEST(Config, DefaultPort) {
    // No --port flag → 6379
    char* argv[] = {(char*)"redis-lite"};
    EXPECT_EQ(parse_config(1, argv).port, 6379);
}

TEST(Config, CustomPort) {
    // --port 6380 → 6380
    char* argv[] = {(char*)"redis-lite", (char*)"--port", (char*)"6380"};
    EXPECT_EQ(parse_config(3, argv).port, 6380);
}

TEST(Config, RandomPort) {
    // --port 9999 → 9999
    char* argv[] = {(char*)"redis-lite", (char*)"--port", (char*)"9999"};
    EXPECT_EQ(parse_config(3, argv).port, 9999);
}

TEST(Config, MissingPortValue) {
    // --port with no value → throws runtime_error
    char* argv[] = {(char*)"redis-lite", (char*)"--port"};
    EXPECT_THROW(parse_config(2, argv), std::runtime_error);
}

TEST(Config, NonNumericPort) {
    // --port abc → throws runtime_error (stoi fails)
    char* argv[] = {(char*)"redis-lite", (char*)"--port", (char*)"abc"};
    EXPECT_THROW(parse_config(3, argv), std::runtime_error);
}

TEST(Config, NegativePort) {
    // --port -1 → throws runtime_error (out of range)
    char* argv[] = {(char*)"redis-lite", (char*)"--port", (char*)"-1"};
    EXPECT_THROW(parse_config(3, argv), std::runtime_error);
}

TEST(Config, ZeroPort) {
    // --port 0 → throws runtime_error (out of range)
    char* argv[] = {(char*)"redis-lite", (char*)"--port", (char*)"0"};
    EXPECT_THROW(parse_config(3, argv), std::runtime_error);
}

TEST(Config, PortTooHigh) {
    // --port 99999 → throws runtime_error (out of range)
    char* argv[] = {(char*)"redis-lite", (char*)"--port", (char*)"99999"};
    EXPECT_THROW(parse_config(3, argv), std::runtime_error);
}

TEST(Config, UnknownFlag) {
    // --foo → throws runtime_error
    char* argv[] = {(char*)"redis-lite", (char*)"--foo"};
    EXPECT_THROW(parse_config(2, argv), std::runtime_error);
}

TEST(Config, UnknownFlagWithValue) {
    // --foo bar → throws runtime_error
    char* argv[] = {(char*)"redis-lite", (char*)"--foo", (char*)"bar"};
    EXPECT_THROW(parse_config(3, argv), std::runtime_error);
}

// --- --replicaof ---

TEST(Config, ReplicaofParsesHostAndPort) {
    char* argv[] = {(char*)"redis-lite", (char*)"--replicaof", (char*)"localhost", (char*)"6379"};
    auto config = parse_config(4, argv);
    ASSERT_TRUE(config.replicaof.has_value());
    EXPECT_EQ(config.replicaof->first, "localhost");
    EXPECT_EQ(config.replicaof->second, 6379);
}

TEST(Config, NoReplicaofDefaultsToNullopt) {
    char* argv[] = {(char*)"redis-lite"};
    auto config = parse_config(1, argv);
    EXPECT_FALSE(config.replicaof.has_value());
}

TEST(Config, ReplicaofMissingValueThrows) {
    char* argv[] = {(char*)"redis-lite", (char*)"--replicaof"};
    EXPECT_THROW(parse_config(2, argv), std::runtime_error);
}

TEST(Config, ReplicaofMissingPortThrows) {
    // Only host, no port arg → argc too low
    char* argv[] = {(char*)"redis-lite", (char*)"--replicaof", (char*)"localhost"};
    EXPECT_THROW(parse_config(3, argv), std::runtime_error);
}

TEST(Config, ReplicaofNonNumericPortThrows) {
    char* argv[] = {(char*)"redis-lite", (char*)"--replicaof", (char*)"localhost", (char*)"abc"};
    EXPECT_THROW(parse_config(4, argv), std::runtime_error);
}

TEST(Config, BothPortAndReplicaof) {
    char* argv[] = {(char*)"redis-lite", (char*)"--port", (char*)"6380",
                    (char*)"--replicaof", (char*)"localhost", (char*)"6379"};
    auto config = parse_config(6, argv);
    EXPECT_EQ(config.port, 6380);
    ASSERT_TRUE(config.replicaof.has_value());
    EXPECT_EQ(config.replicaof->first, "localhost");
    EXPECT_EQ(config.replicaof->second, 6379);
}

TEST(Config, ReplicaofPortOutOfRangeThrows) {
    char* argv[] = {(char*)"redis-lite", (char*)"--replicaof", (char*)"localhost", (char*)"99999"};
    EXPECT_THROW(parse_config(4, argv), std::runtime_error);
}