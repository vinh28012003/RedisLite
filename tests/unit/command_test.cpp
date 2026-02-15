#include <gtest/gtest.h>
#include "command.hpp"

TEST(Command, PingReturnPong) {
    auto response = command::execute({"PING"});
    EXPECT_EQ(response, "+PONG\r\n");
}

TEST(Command, EchoReturnArgument) {
    auto response = command::execute({"ECHO", "hey"});
    EXPECT_EQ(response, "$3\r\nhey\r\n");
}

TEST(Command, EchoCaseInsensitive) {
    EXPECT_EQ(command::execute({"echo", "test"}), "$4\r\ntest\r\n");
    EXPECT_EQ(command::execute({"EcHo", "test"}), "$4\r\ntest\r\n");
}

TEST(Command, EmptyArgsReturnError) {
    auto response = command::execute({});
    EXPECT_EQ(response, "-ERR no command\r\n");
}

TEST(Command, EchoMissingArgReturnsError) {
    auto response = command::execute({"ECHO"});
    EXPECT_EQ(response, "-ERR wrong number of arguments for 'echo' command\r\n");
}

TEST(Command, UnknownCommandResturnsError) {
    auto response = command::execute({"FOOBAR"});
    EXPECT_EQ(response, "-ERR unknown command 'FOOBAR'\r\n");
}

