#include <gtest/gtest.h>
#include "command.hpp"

TEST(Command, PingReturnPong) {
    Store store;
    auto response = command::execute({"PING"}, store);
    EXPECT_EQ(response, "+PONG\r\n");
}

TEST(Command, EchoReturnArgument) {
    Store store;
    auto response = command::execute({"ECHO", "hey"}, store);
    EXPECT_EQ(response, "$3\r\nhey\r\n");
}

TEST(Command, EchoCaseInsensitive) {
    Store store;
    EXPECT_EQ(command::execute({"echo", "test"}, store), "$4\r\ntest\r\n");
    EXPECT_EQ(command::execute({"EcHo", "test"}, store), "$4\r\ntest\r\n");
}

TEST(Command, EmptyArgsReturnError) {
    Store store;
    auto response = command::execute({}, store);
    EXPECT_EQ(response, "-ERR no command\r\n");
}

TEST(Command, EchoMissingArgReturnsError) {
    Store store;
    auto response = command::execute({"ECHO"}, store);
    EXPECT_EQ(response, "-ERR wrong number of arguments for 'echo' command\r\n");
}

TEST(Command, UnknownCommandResturnsError) {
    Store store;
    auto response = command::execute({"FOOBAR"}, store);
    EXPECT_EQ(response, "-ERR unknown command 'FOOBAR'\r\n");
}

TEST(Command, SetReturnOk) {
    Store store;
    auto response = command::execute({"SET", "foo", "bar"}, store);
    EXPECT_EQ(response, "+OK\r\n");
}

TEST(Command, GetReturnValue) {
    Store store;
    command::execute({"SET", "foo", "bar"}, store);
    auto response = command::execute({"GET", "foo"}, store);
    EXPECT_EQ(response, "$3\r\nbar\r\n");
}

TEST(Command, GetMissingKeyReturnsNull) {
    Store store;
    auto response = command::execute({"GET", "nonexistent"}, store);
    EXPECT_EQ(response, "$-1\r\n");
}

TEST(Command, SetOverwritesExistingKey) {
    Store store;
    command::execute({"SET", "key", "old"}, store);
    command::execute({"SET", "key", "new"}, store);
    auto response = command::execute({"GET", "key"}, store);
    EXPECT_EQ(response, "$3\r\nnew\r\n");
}
