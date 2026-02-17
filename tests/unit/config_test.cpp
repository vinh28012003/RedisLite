#include <gtest/gtest.h>
#include "config.hpp"

TEST(Config, DefaultPort) {
    // No --port flag → 6379
    char* argv[] = {(char*)"redis-lite"};
    EXPECT_EQ(parse_port(1, argv), 6379);
}

TEST(Config, CustomPort) {
    // --port 6380 → 6380
    char* argv[] = {(char*)"redis-lite", (char*)"--port", (char*)"6380"};
    EXPECT_EQ(parse_port(3, argv), 6380);
}

TEST(Config, RandomPort) {
    // --port 9999 → 9999
    char* argv[] = {(char*)"redis-lite", (char*)"--port", (char*)"9999"};
    EXPECT_EQ(parse_port(3, argv), 9999);
}

TEST(Config, MissingPortValue) {
    // --port with no value → throws runtime_error
    char* argv[] = {(char*)"redis-lite", (char*)"--port"};
    EXPECT_THROW(parse_port(2, argv), std::runtime_error);
}

TEST(Config, NonNumericPort) {
    // --port abc → throws runtime_error (stoi fails)
    char* argv[] = {(char*)"redis-lite", (char*)"--port", (char*)"abc"};
    EXPECT_THROW(parse_port(3, argv), std::runtime_error);
}

TEST(Config, NegativePort) {
    // --port -1 → throws runtime_error (out of range)
    char* argv[] = {(char*)"redis-lite", (char*)"--port", (char*)"-1"};
    EXPECT_THROW(parse_port(3, argv), std::runtime_error);
}

TEST(Config, ZeroPort) {
    // --port 0 → throws runtime_error (out of range)
    char* argv[] = {(char*)"redis-lite", (char*)"--port", (char*)"0"};
    EXPECT_THROW(parse_port(3, argv), std::runtime_error);
}

TEST(Config, PortTooHigh) {
    // --port 99999 → throws runtime_error (out of range)
    char* argv[] = {(char*)"redis-lite", (char*)"--port", (char*)"99999"};
    EXPECT_THROW(parse_port(3, argv), std::runtime_error);
}

TEST(Config, UnknownFlag) {
    // --foo → throws runtime_error
    char* argv[] = {(char*)"redis-lite", (char*)"--foo"};
    EXPECT_THROW(parse_port(2, argv), std::runtime_error);
}

TEST(Config, UnknownFlagWithValue) {
    // --foo bar → throws runtime_error
    char* argv[] = {(char*)"redis-lite", (char*)"--foo", (char*)"bar"};
    EXPECT_THROW(parse_port(3, argv), std::runtime_error);
}