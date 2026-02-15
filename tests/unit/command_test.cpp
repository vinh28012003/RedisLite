#include <gtest/gtest.h>                                                                                
#include "command.hpp"                                                                                  
#include <thread>                                                                                       
#include <chrono>                                                                                       
                
// --- Dispatch: empty/unknown ---                                                                      
                                                                                                        
TEST(Command, EmptyArgsReturnError) {
    Store store;
    auto response = command::execute({}, store);
    EXPECT_EQ(response, "-ERR no command\r\n");
}

TEST(Command, UnknownCommandResturnsError) {
    Store store;
    auto response = command::execute({"FOOBAR"}, store);
    EXPECT_EQ(response, "-ERR unknown command 'FOOBAR'\r\n");
}

// --- PING ---

TEST(Command, PingReturnPong) {
    Store store;
    auto response = command::execute({"PING"}, store);
    EXPECT_EQ(response, "+PONG\r\n");
}

// --- ECHO ---

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

TEST(Command, EchoMissingArgReturnsError) {
    Store store;
    auto response = command::execute({"ECHO"}, store);
    EXPECT_EQ(response, "-ERR wrong number of arguments for 'echo' command\r\n");
}

// --- SET ---

TEST(Command, SetReturnOk) {
    Store store;
    auto response = command::execute({"SET", "foo", "bar"}, store);
    EXPECT_EQ(response, "+OK\r\n");
}

TEST(Command, SetOverwritesExistingKey) {
    Store store;
    command::execute({"SET", "key", "old"}, store);
    command::execute({"SET", "key", "new"}, store);
    auto response = command::execute({"GET", "key"}, store);
    EXPECT_EQ(response, "$3\r\nnew\r\n");
}

TEST(Command, SetMissingArgsReturnsError) {
    Store store;
    auto response = command::execute({"SET", "foo"}, store);
    EXPECT_EQ(response, "-ERR wrong number of arguments for 'set' command\r\n");
}

// --- GET ---

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

TEST(Command, GetMissingArgsReturnsError) {
    Store store;
    auto response = command::execute({"GET"}, store);
    EXPECT_EQ(response, "-ERR wrong number of arguments for 'get' command\r\n");
}

// --- Expiration (PX/EX) ---

TEST(Command, SetWithPxExpires) {
    Store store;
    command::execute({"SET", "foo", "bar", "PX", "50"}, store);
    EXPECT_EQ(command::execute({"GET", "foo"}, store), "$3\r\nbar\r\n");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(command::execute({"GET", "foo"}, store), "$-1\r\n");
}

TEST(Command, SetWithExExpires) {
    Store store;
    command::execute({"SET", "foo", "bar", "EX", "1"}, store);
    EXPECT_EQ(command::execute({"GET", "foo"}, store), "$3\r\nbar\r\n");
}

TEST(Command, SetClearsPreviousTtl) {
    Store store;
    command::execute({"SET", "foo", "bar", "PX", "50"}, store);
    command::execute({"SET", "foo", "bar"}, store); //no PX - clears TTL
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(command::execute({"GET", "foo"}, store), "$3\r\nbar\r\n"); //still alive
}

TEST(Command, SetPxCaseInsensitive) {
    Store store;
    command::execute({"SET", "foo", "bar", "px", "50"}, store);
    EXPECT_EQ(command::execute({"GET", "foo"}, store), "$3\r\nbar\r\n");
}

TEST(Command, SetExCaseInsensitive) {
    Store store;
    command::execute({"SET", "foo", "bar", "ex", "1"}, store);
    EXPECT_EQ(command::execute({"GET", "foo"}, store), "$3\r\nbar\r\n");
}

TEST(Command, SetWithInvalidPxReturnsError) {
    Store store;
    auto response = command::execute({"SET", "foo", "bar", "PX", "notanumber"}, store);
    EXPECT_EQ(response, "-ERR value is not an integer or out of range\r\n");
}

// --- Store: active expiration (evict_expired) ---

TEST(Command, EvictExpiredRemovesKeys) {
    Store store;
    store.set("a", "1", 50);
    store.set("b", "2", 50);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    store.evict_expired();
    EXPECT_EQ(store.size(), 0);  // no get() called — proves active path
}

TEST(Command, EvictExpiredKeepsLiveKeys) {
    Store store;
    store.set("alive", "yes");
    store.set("dead", "no", 50);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    store.evict_expired();
    EXPECT_EQ(store.size(), 1);  // only "alive" remains
}