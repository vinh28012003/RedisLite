#include <gtest/gtest.h>                                                                                
#include "command.hpp"                                                                                  
#include <thread>                                                                                       
#include <chrono>                                                                                       
#include "replication_info.hpp"
#include "resp_parser.hpp"

static const ReplicationInfo DEFAULT_REPL{"master", "8371b4fb1155b71f4a04d3e1bc3e18c4a990aeeb", 0};

// --- Dispatch: empty/unknown ---                                                                      
                                                                                                        
TEST(Command, EmptyArgsReturnError) {
    Store store;
    auto response = command::execute({}, store, DEFAULT_REPL);
    EXPECT_EQ(response, "-ERR no command\r\n");
}

TEST(Command, UnknownCommandResturnsError) {
    Store store;
    auto response = command::execute({"FOOBAR"}, store, DEFAULT_REPL);
    EXPECT_EQ(response, "-ERR unknown command 'FOOBAR'\r\n");
}

// --- PING ---

TEST(Command, PingReturnPong) {
    Store store;
    auto response = command::execute({"PING"}, store, DEFAULT_REPL);
    EXPECT_EQ(response, "+PONG\r\n");
}

// --- ECHO ---

TEST(Command, EchoReturnArgument) {
    Store store;
    auto response = command::execute({"ECHO", "hey"}, store, DEFAULT_REPL);
    EXPECT_EQ(response, "$3\r\nhey\r\n");
}

TEST(Command, EchoCaseInsensitive) {
    Store store;
    EXPECT_EQ(command::execute({"echo", "test"}, store, DEFAULT_REPL), "$4\r\ntest\r\n");
    EXPECT_EQ(command::execute({"EcHo", "test"}, store, DEFAULT_REPL), "$4\r\ntest\r\n");
}

TEST(Command, EchoMissingArgReturnsError) {
    Store store;
    auto response = command::execute({"ECHO"}, store, DEFAULT_REPL);
    EXPECT_EQ(response, "-ERR wrong number of arguments for 'echo' command\r\n");
}

// --- SET ---

TEST(Command, SetReturnOk) {
    Store store;
    auto response = command::execute({"SET", "foo", "bar"}, store, DEFAULT_REPL);
    EXPECT_EQ(response, "+OK\r\n");
}

TEST(Command, SetOverwritesExistingKey) {
    Store store;
    command::execute({"SET", "key", "old"}, store, DEFAULT_REPL);
    command::execute({"SET", "key", "new"}, store, DEFAULT_REPL);
    auto response = command::execute({"GET", "key"}, store, DEFAULT_REPL);
    EXPECT_EQ(response, "$3\r\nnew\r\n");
}

TEST(Command, SetMissingArgsReturnsError) {
    Store store;
    auto response = command::execute({"SET", "foo"}, store, DEFAULT_REPL);
    EXPECT_EQ(response, "-ERR wrong number of arguments for 'set' command\r\n");
}

// --- GET ---

TEST(Command, GetReturnValue) {
    Store store;
    command::execute({"SET", "foo", "bar"}, store, DEFAULT_REPL);
    auto response = command::execute({"GET", "foo"}, store, DEFAULT_REPL);
    EXPECT_EQ(response, "$3\r\nbar\r\n");
}

TEST(Command, GetMissingKeyReturnsNull) {
    Store store;
    auto response = command::execute({"GET", "nonexistent"}, store, DEFAULT_REPL);
    EXPECT_EQ(response, "$-1\r\n");
}

TEST(Command, GetMissingArgsReturnsError) {
    Store store;
    auto response = command::execute({"GET"}, store, DEFAULT_REPL);
    EXPECT_EQ(response, "-ERR wrong number of arguments for 'get' command\r\n");
}

// --- Expiration (PX/EX) ---

TEST(Command, SetWithPxExpires) {
    Store store;
    command::execute({"SET", "foo", "bar", "PX", "50"}, store, DEFAULT_REPL);
    EXPECT_EQ(command::execute({"GET", "foo"}, store, DEFAULT_REPL), "$3\r\nbar\r\n");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(command::execute({"GET", "foo"}, store, DEFAULT_REPL), "$-1\r\n");
}

TEST(Command, SetWithExExpires) {
    Store store;
    command::execute({"SET", "foo", "bar", "EX", "1"}, store, DEFAULT_REPL);
    EXPECT_EQ(command::execute({"GET", "foo"}, store, DEFAULT_REPL), "$3\r\nbar\r\n");
}

TEST(Command, SetClearsPreviousTtl) {
    Store store;
    command::execute({"SET", "foo", "bar", "PX", "50"}, store, DEFAULT_REPL);
    command::execute({"SET", "foo", "bar"}, store, DEFAULT_REPL); //no PX - clears TTL
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(command::execute({"GET", "foo"}, store, DEFAULT_REPL), "$3\r\nbar\r\n"); //still alive
}

TEST(Command, SetPxCaseInsensitive) {
    Store store;
    command::execute({"SET", "foo", "bar", "px", "50"}, store, DEFAULT_REPL);
    EXPECT_EQ(command::execute({"GET", "foo"}, store, DEFAULT_REPL), "$3\r\nbar\r\n");
}

TEST(Command, SetExCaseInsensitive) {
    Store store;
    command::execute({"SET", "foo", "bar", "ex", "1"}, store, DEFAULT_REPL);
    EXPECT_EQ(command::execute({"GET", "foo"}, store, DEFAULT_REPL), "$3\r\nbar\r\n");
}

TEST(Command, SetWithInvalidPxReturnsError) {
    Store store;
    auto response = command::execute({"SET", "foo", "bar", "PX", "notanumber"}, store, DEFAULT_REPL);
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

// --- INFO ---

TEST(Command, InfoReplicationReturnsMasterRole) {
    Store store;
    ReplicationInfo info{"master"};
    auto response = command::execute({"INFO", "replication"}, store, info);
    EXPECT_NE(response.find("role:master"), std::string::npos);
}

TEST(Command, InfoNoArgReturnsMasterRole) {
    Store store;
    auto response = command::execute({"INFO"}, store, DEFAULT_REPL);
    EXPECT_NE(response.find("role:master"), std::string::npos);
}

TEST(Command, InfoCaseInsensitive) {
    Store store;
    auto response = command::execute({"info", "Replication"}, store, DEFAULT_REPL);
    EXPECT_NE(response.find("role:master"), std::string::npos);
}

TEST(Command, InfoUnknownSectionReturnsEmptyBulk) {
    Store store;
    auto response = command::execute({"INFO", "memory"}, store, DEFAULT_REPL);
    EXPECT_EQ(response, "$0\r\n\r\n");
}

TEST(Command, InfoReplicationReturnsReplicaRole) {
    Store store;
    ReplicationInfo replica_info{"worker", "8371b4fb1155b71f4a04d3e1bc3e18c4a990aeeb", 0};
    auto response = command::execute({"INFO", "replication"}, store, replica_info);
    EXPECT_NE(response.find("role:worker"), std::string::npos);
}

TEST(Command, InfoReplicationReturnsBulkStringFormat) {
    Store store;
    auto response = command::execute({"INFO", "replication"}, store, DEFAULT_REPL);
    std::string expected = "role:master\r\n"
                           "master_replid:8371b4fb1155b71f4a04d3e1bc3e18c4a990aeeb\r\n"
                           "master_repl_offset:0";
    EXPECT_EQ(response, resp::encode_bulk_string(expected));
}

TEST(Command, InfoIgnoresExtraArguments) {
    Store store;
    auto response = command::execute({"INFO", "replication", "junk", "morejunk"}, store, DEFAULT_REPL);
    EXPECT_NE(response.find("role:master"), std::string::npos);
}

TEST(Command, InfoContainsReplid) {
    Store store;
    auto response = command::execute({"INFO", "replication"}, store, DEFAULT_REPL);
    EXPECT_NE(response.find("master_replid:8371b4fb1155b71f4a04d3e1bc3e18c4a990aeeb"),
std::string::npos);
}

TEST(Command, InfoContainsReplOffset) {
    Store store;
    auto response = command::execute({"INFO", "replication"}, store, DEFAULT_REPL);
    EXPECT_NE(response.find("master_repl_offset:0"), std::string::npos);
}

// -- Test Handshake
TEST(Command, ReplconfReturnsOk) {
    Store store;
    ReplicationInfo repl{"master", "abc", 0};
    auto result = command::execute({"REPLCONF", "listening-port", "6380"}, store, repl);
    EXPECT_EQ(result, "+OK\r\n");
}

TEST(Command, ReplconfCapaReturnsOk) {
    Store store;
    ReplicationInfo repl{"master", "abc", 0};
    auto result = command::execute({"REPLCONF", "capa", "psync2"}, store, repl);
    EXPECT_EQ(result, "+OK\r\n");
}

TEST(Command, PsyncReturnsFulresync) {
    Store store;
    ReplicationInfo repl_info{"master", "8371445fff7e1767aab63d5e534e3492a8ee2ee6", 0};
    auto result = command::execute({"PSYNC", "?", "-1"}, store, repl_info);
    EXPECT_EQ(result, "+FULLRESYNC 8371445fff7e1767aab63d5e534e3492a8ee2ee6 0\r\n");
}

TEST(Command, PsyncIgnoresArgsForNow) {
    Store store;
    ReplicationInfo repl_info{"master", "8371445fff7e1767aab63d5e534e3492a8ee2ee6", 0};
    auto result = command::execute({"PSYNC", "abc123", "100"}, store, repl_info);
    EXPECT_EQ(result, "+FULLRESYNC 8371445fff7e1767aab63d5e534e3492a8ee2ee6 0\r\n");
}