#include <gtest/gtest.h>
#include "resp_parser.hpp"

// --- parse() tests ---

TEST(RespParser, ParsePing) {
    const char* input = "*1\r\n$4\r\nPING\r\n";
    auto result = resp::parse(input, strlen(input));
    ASSERT_EQ(result.args.size(), 1);
    EXPECT_EQ(result.args[0], "PING");
    EXPECT_GT(result.bytes_consumed, 0); 
}

TEST(RespParser, ParseEchoWithArg) {
    const char* input = "*2\r\n$4\r\nECHO\r\n$3\r\nhey\r\n";
    auto result = resp::parse(input, strlen(input));
    ASSERT_EQ(result.args.size(), 2);
    EXPECT_EQ(result.args[0], "ECHO");
    EXPECT_EQ(result.args[1], "hey");
}

TEST(RespParser, ParseMultipleArgs) {
    const char* input = "*3\r\n$3\r\nSET\r\n$4\r\nname\r\n$5\r\nAlice\r\n";
    auto result = resp::parse(input, strlen(input));
    ASSERT_EQ(result.args.size(), 3);
    EXPECT_EQ(result.args[0], "SET");
    EXPECT_EQ(result.args[1], "name");
    EXPECT_EQ(result.args[2], "Alice");
}

TEST(RespParser, EmptyBuffer) {
    auto result = resp::parse("", 0);
    EXPECT_TRUE(result.args.empty());
    EXPECT_EQ(result.bytes_consumed, 0); 
}

TEST(RespParser, InvalidNoAsterisk) {
    const char* input = "PING\r\n";
    auto result = resp::parse(input, strlen(input));
    EXPECT_TRUE(result.args.empty());
    EXPECT_EQ(result.bytes_consumed, 0);
}

// --- encode_bulk_string() tests ---

TEST(RespParser, EncodeBulkString) {
    EXPECT_EQ(resp::encode_bulk_string("hey"), "$3\r\nhey\r\n");
}

TEST(RespParser, EncodeBulkStringEmpty) {
    EXPECT_EQ(resp::encode_bulk_string(""), "$0\r\n\r\n");
}

TEST(RespParser, IncompleteCommand) {
    const char* input = "*3\r\n$3\r\nSET\r\n$3\r\n";  // cut off mid-command
    auto result = resp::parse(input, strlen(input));
    EXPECT_TRUE(result.args.empty());
    EXPECT_EQ(result.bytes_consumed, 0);
}

TEST(RespParser, IncompleteArrayHeader) {
    const char* input = "*3\r";  // no \n yet
    auto result = resp::parse(input, strlen(input));
    EXPECT_TRUE(result.args.empty());
    EXPECT_EQ(result.bytes_consumed, 0);
}

TEST(RespParser, EncodeArraySingleElement) {                                                            
    // {"PING"} -> "*1\r\n$4\r\nPING\r\n"                                                               
    auto result = resp::encode_array({"PING"});                                                       
    EXPECT_EQ(result, "*1\r\n$4\r\nPING\r\n");                                                          
}                                                                                                       

TEST(RespParser, EncodeArrayMultipleElements) {
    // {"REPLCONF", "listening-port", "6380"}
    auto result = resp::encode_array({"REPLCONF", "listening-port", "6380"});
    EXPECT_EQ(result, "*3\r\n$8\r\nREPLCONF\r\n$14\r\nlistening-port\r\n$4\r\n6380\r\n");
}

TEST(RespParser, EncodeArrayEmpty) {
    auto result = resp::encode_array({});
    EXPECT_EQ(result, "*0\r\n");
}

// ═══════════════════════════════════════════════════════════════════
// Malformed input — parser must return gracefully, not crash (stoi)
// ═══════════════════════════════════════════════════════════════════

TEST(RespParser, ParseNonNumericArrayCount) {
    // *xyz is not a valid count — parser should return empty, not throw
    const char* input = "*xyz\r\n$4\r\nPING\r\n";
    auto result = resp::parse(input, strlen(input));
    EXPECT_TRUE(result.args.empty());
    EXPECT_EQ(result.bytes_consumed, 0);
}

TEST(RespParser, ParseNonNumericBulkLength) {
    // $abc is not a valid length — parser should return empty, not throw
    const char* input = "*1\r\n$abc\r\ndata\r\n";
    auto result = resp::parse(input, strlen(input));
    EXPECT_TRUE(result.args.empty());
    EXPECT_EQ(result.bytes_consumed, 0);
}

TEST(RespParser, ParseNegativeBulkLength) {
    // $-1 (null bulk string) — should not cause integer underflow
    const char* input = "*1\r\n$-1\r\n";
    auto result = resp::parse(input, strlen(input));
    EXPECT_TRUE(result.args.empty());
    EXPECT_EQ(result.bytes_consumed, 0);
}

// ═══════════════════════════════════════════════════════════════════
// Edge cases — valid RESP that exercises boundary conditions
// ═══════════════════════════════════════════════════════════════════

TEST(RespParser, ParseZeroElementArray) {
    // *0\r\n — valid RESP: empty array, no elements
    const char* input = "*0\r\n";
    auto result = resp::parse(input, strlen(input));
    EXPECT_TRUE(result.args.empty());
    EXPECT_EQ(result.bytes_consumed, 4);
}

TEST(RespParser, ParseEmptyBulkStringInCommand) {
    // Command with a zero-length bulk string argument
    const char* input = "*2\r\n$3\r\nSET\r\n$0\r\n\r\n";
    auto result = resp::parse(input, strlen(input));
    ASSERT_EQ(result.args.size(), 2);
    EXPECT_EQ(result.args[0], "SET");
    EXPECT_EQ(result.args[1], "");
    EXPECT_GT(result.bytes_consumed, 0);
}

TEST(RespParser, ParsePipeliningOnlyConsumesFirstCommand) {
    // Two commands back-to-back — parse should consume only the first
    const char* input = "*1\r\n$4\r\nPING\r\n*1\r\n$4\r\nPING\r\n";
    auto result = resp::parse(input, strlen(input));
    ASSERT_EQ(result.args.size(), 1);
    EXPECT_EQ(result.args[0], "PING");
    // bytes_consumed should be length of first command only
    size_t first_cmd_len = strlen("*1\r\n$4\r\nPING\r\n");
    EXPECT_EQ(result.bytes_consumed, first_cmd_len);
}

