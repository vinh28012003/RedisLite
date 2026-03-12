#include <gtest/gtest.h>                                                                                
#include "command.hpp"                                                                                  
#include <thread>                                                                                       
#include <chrono>                                                                                       
#include "replication_info.hpp"
#include "resp_parser.hpp"

static const ReplicationInfo DEFAULT_REPL{"master", "8371b4fb1155b71f4a04d3e1bc3e18c4a990aeeb", 0, "", 0, std::string(40, '0'), -1};

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

TEST(Command, SetWithInvalidExReturnsError) {
    Store store;
    auto response = command::execute({"SET", "foo", "bar", "EX", "notanumber"}, store, DEFAULT_REPL);
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
    ReplicationInfo replica_info{"worker", "8371b4fb1155b71f4a04d3e1bc3e18c4a990aeeb", 0, "localhost", 6379};
    auto response = command::execute({"INFO", "replication"}, store, replica_info);
    EXPECT_NE(response.find("role:worker"), std::string::npos);
}

TEST(Command, InfoReplicationReturnsBulkStringFormat) {
    Store store;
    auto response = command::execute({"INFO", "replication"}, store, DEFAULT_REPL);
    std::string expected = "role:master\r\n"
                           "master_replid:8371b4fb1155b71f4a04d3e1bc3e18c4a990aeeb\r\n"
                           "master_repl_offset:0\r\n"
                           "master_replid2:" + std::string(40, '0') + "\r\n"
                           "second_repl_offset:-1";
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

    // Verify FULLRESYNC prefix
    EXPECT_EQ(result.substr(0, 56), "+FULLRESYNC 8371445fff7e1767aab63d5e534e3492a8ee2ee6 0\r\n");

    // Verify RDB transfer: $88\r\n + 88 binary bytes, no trailing \r\n
    std::string rdb_part = result.substr(56);
    EXPECT_EQ(rdb_part.substr(0, 5), "$88\r\n");
    EXPECT_EQ(rdb_part.size(), 5 + 88);  // "$88\r\n" + 88 bytes
    EXPECT_EQ(static_cast<uint8_t>(rdb_part[5]), 0x52);  // 'R' — first byte of "REDIS"
    EXPECT_EQ(static_cast<uint8_t>(rdb_part[6]), 0x45);  // 'E'
}

TEST(Command, PsyncIgnoresArgsForNow) {
    Store store;
    ReplicationInfo repl_info{"master", "8371445fff7e1767aab63d5e534e3492a8ee2ee6", 0};
    auto result = command::execute({"PSYNC", "abc123", "100"}, store, repl_info);

    EXPECT_TRUE(result.find("+FULLRESYNC 8371445fff7e1767aab63d5e534e3492a8ee2ee6 0\r\n") == 0);
    // Total: 56 (FULLRESYNC line) + 5 ($88\r\n) + 88 (binary) = 149 bytes
    EXPECT_EQ(result.size(), 149);
}


// --- is_write_command ---

TEST(Command, IsWriteCommandSet) {
    EXPECT_TRUE(command::is_write_command("SET"));
}

TEST(Command, IsWriteCommandDel) {
    EXPECT_TRUE(command::is_write_command("DEL"));
}

TEST(Command, IsWriteCommandCaseInsensitive) {
    EXPECT_TRUE(command::is_write_command("set"));
    EXPECT_TRUE(command::is_write_command("del"));
}

TEST(Command, IsWriteCommandGetReturnsFalse) {
    EXPECT_FALSE(command::is_write_command("GET"));
}

TEST(Command, IsWriteCommandPingReturnsFalse) {
    EXPECT_FALSE(command::is_write_command("PING"));
}

// --- REPLCONF GETACK (stages 14-15) ---                                                           
                  
TEST(Command, ReplconfGetackReturnsAckWithZeroOffset) {                                             
    Store store;                                                                                    
    ReplicationInfo repl{"replica", "abc123", 0};
    auto result = command::execute({"REPLCONF", "GETACK", "*"}, store, repl);
    EXPECT_EQ(result, "*3\r\n$8\r\nREPLCONF\r\n$3\r\nACK\r\n$1\r\n0\r\n");
}

TEST(Command, ReplconfGetackReturnsAckWithNonZeroOffset) {
    Store store;
    ReplicationInfo repl{"replica", "abc123", 154};
    auto result = command::execute({"REPLCONF", "GETACK", "*"}, store, repl);
    EXPECT_EQ(result, "*3\r\n$8\r\nREPLCONF\r\n$3\r\nACK\r\n$3\r\n154\r\n");
}

TEST(Command, ReplconfGetackCaseInsensitive) {
    Store store;
    ReplicationInfo repl{"replica", "abc123", 0};
    auto result = command::execute({"replconf", "getack", "*"}, store, repl);
    EXPECT_EQ(result, "*3\r\n$8\r\nREPLCONF\r\n$3\r\nACK\r\n$1\r\n0\r\n");
}

TEST(Command, ReplconfListeningPortStillReturnsOkAfterGetackChange) {
    Store store;
    ReplicationInfo repl{"master", "abc123", 0};
    auto result = command::execute({"REPLCONF", "listening-port", "6380"}, store, repl);
    EXPECT_EQ(result, "+OK\r\n");
}

// --- WAIT (stages 16-18) ---

TEST(Command, WaitReturnsEmptyForServerHandling) {
    Store store;
    auto result = command::execute({"WAIT", "1", "500"}, store, DEFAULT_REPL);
    EXPECT_EQ(result, "");  // Empty = server owns WAIT logic
}

TEST(Command, WaitCaseInsensitive) {
    Store store;
    auto result = command::execute({"wait", "0", "0"}, store, DEFAULT_REPL);
    EXPECT_EQ(result, "");
}

TEST(Command, WaitMissingArgsReturnsError) {
    Store store;
    auto result = command::execute({"WAIT"}, store, DEFAULT_REPL);
    EXPECT_EQ(result, "-ERR wrong number of arguments for 'wait' command\r\n");
}

TEST(Command, WaitMissingTimeoutReturnsError) {
    Store store;
    auto result = command::execute({"WAIT", "1"}, store, DEFAULT_REPL);
    EXPECT_EQ(result, "-ERR wrong number of arguments for 'wait' command\r\n");
}

TEST(Command, WaitNonNumericReturnsError) {
    Store store;
    auto result = command::execute({"WAIT", "abc", "def"}, store, DEFAULT_REPL);
    EXPECT_EQ(result, "-ERR value is not an integer or out of range\r\n");
}

// --- Replica read-only ---

TEST(Command, ReplicaRejectsSetWithReadonlyError) {
    Store store;
    ReplicationInfo repl{"worker", "abc", 0};
    auto result = command::execute({"SET", "foo", "bar"}, store, repl);
    EXPECT_EQ(result, "-READONLY You can't write against a read only replica.\r\n");
}

TEST(Command, ReplicaRejectsDelWithReadonlyError) {
    Store store;
    ReplicationInfo repl{"worker", "abc", 0};
    auto result = command::execute({"DEL", "foo"}, store, repl);
    EXPECT_EQ(result, "-READONLY You can't write against a read only replica.\r\n");
}

TEST(Command, ReplicaAllowsGetCommand) {
    Store store;
    ReplicationInfo repl{"worker", "abc", 0};
    auto result = command::execute({"GET", "foo"}, store, repl);
    EXPECT_EQ(result, "$-1\r\n");
}

TEST(Command, MasterAllowsSetCommand) {
    Store store;
    auto result = command::execute({"SET", "foo", "bar"}, store, DEFAULT_REPL);
    EXPECT_EQ(result, "+OK\r\n");
}

TEST(Command, ReplicaAllowsSetFromMaster) {
    Store store;
    ReplicationInfo repl{"worker", "abc", 0};
    auto result = command::execute({"SET", "foo", "bar"}, store, repl, true);
    EXPECT_EQ(result, "+OK\r\n");
}

TEST(Command, ReplicaRejectsSetCaseInsensitive) {
    Store store;
    ReplicationInfo repl{"worker", "abc", 0};
    auto result = command::execute({"set", "foo", "bar"}, store, repl);
    EXPECT_EQ(result, "-READONLY You can't write against a read only replica.\r\n");
}

// --- ROLE command ---

TEST(Command, RoleMasterReturnsArrayWithMasterAndOffset) {
    Store store;
    auto result = command::execute({"ROLE"}, store, DEFAULT_REPL);
    // *3\r\n $6\r\nmaster\r\n :0\r\n *0\r\n
    EXPECT_EQ(result, "*3\r\n$6\r\nmaster\r\n:0\r\n*0\r\n");
}

TEST(Command, RoleMasterWithNonZeroOffset) {
    Store store;
    ReplicationInfo repl{"master", "abc", 154};
    auto result = command::execute({"ROLE"}, store, repl);
    EXPECT_NE(result.find(":154\r\n"), std::string::npos);
}

TEST(Command, RoleReplicaReturnsSlave) {
    Store store;
    ReplicationInfo repl{"worker", "abc", 0, "localhost", 6379};
    auto result = command::execute({"ROLE"}, store, repl);
    EXPECT_NE(result.find("$5\r\nslave\r\n"), std::string::npos);
    EXPECT_NE(result.find("localhost"), std::string::npos);
    EXPECT_NE(result.find(":6379\r\n"), std::string::npos);
    EXPECT_NE(result.find("connected"), std::string::npos);
}

TEST(Command, RoleReplicaWithNonZeroOffset) {
    Store store;
    ReplicationInfo repl{"worker", "abc", 250, "master-host", 6379};
    auto result = command::execute({"ROLE"}, store, repl);
    EXPECT_NE(result.find(":250\r\n"), std::string::npos);
}

TEST(Command, RoleCaseInsensitive) {
    Store store;
    auto result = command::execute({"role"}, store, DEFAULT_REPL);
    EXPECT_EQ(result, "*3\r\n$6\r\nmaster\r\n:0\r\n*0\r\n");
}

// --- REPLICAOF command validation ---

TEST(Command, ReplicaofNoOneReturnsEmpty) {
    Store store;
    auto result = command::execute({"REPLICAOF", "NO", "ONE"}, store, DEFAULT_REPL);
    EXPECT_EQ(result, "");
}

TEST(Command, ReplicaofHostPortReturnsEmpty) {
    Store store;
    auto result = command::execute({"REPLICAOF", "localhost", "6379"}, store, DEFAULT_REPL);
    EXPECT_EQ(result, "");
}

TEST(Command, ReplicaofMissingArgsReturnsError) {
    Store store;
    auto result = command::execute({"REPLICAOF"}, store, DEFAULT_REPL);
    EXPECT_EQ(result, "-ERR wrong number of arguments for 'replicaof' command\r\n");
}

TEST(Command, ReplicaofInvalidPortReturnsError) {
    Store store;
    auto result = command::execute({"REPLICAOF", "localhost", "99999"}, store, DEFAULT_REPL);
    EXPECT_EQ(result, "-ERR Invalid master port\r\n");
}

TEST(Command, ReplicaofNonNumericPortReturnsError) {
    Store store;
    auto result = command::execute({"REPLICAOF", "localhost", "abc"}, store, DEFAULT_REPL);
    EXPECT_EQ(result, "-ERR Invalid master port\r\n");
}

TEST(Command, ReplicaofCaseInsensitive) {
    Store store;
    auto result = command::execute({"replicaof", "no", "one"}, store, DEFAULT_REPL);
    EXPECT_EQ(result, "");
}

TEST(Command, ReplicaofZeroPortReturnsError) {
    Store store;
    auto result = command::execute({"REPLICAOF", "localhost", "0"}, store, DEFAULT_REPL);
    EXPECT_EQ(result, "-ERR Invalid master port\r\n");
}

// --- generate_replid ---

TEST(ReplicationInfo, GenerateReplidReturns40Chars) {
    auto id = generate_replid();
    EXPECT_EQ(id.size(), 40);
}

TEST(ReplicationInfo, GenerateReplidAllHex) {
    auto id = generate_replid();
    for (char c : id) {
        EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
    }
}

TEST(ReplicationInfo, GenerateReplidUnique) {
    auto id1 = generate_replid();
    auto id2 = generate_replid();
    EXPECT_NE(id1, id2);
}

// --- INFO with dual replication IDs ---

TEST(Command, InfoContainsReplid2) {
    Store store;
    auto response = command::execute({"INFO", "replication"}, store, DEFAULT_REPL);
    EXPECT_NE(response.find("master_replid2:" + std::string(40, '0')), std::string::npos);
}

TEST(Command, InfoContainsSecondReplOffset) {
    Store store;
    auto response = command::execute({"INFO", "replication"}, store, DEFAULT_REPL);
    EXPECT_NE(response.find("second_repl_offset:-1"), std::string::npos);
}

TEST(Command, InfoReportsNonDefaultReplid2) {
    Store store;
    ReplicationInfo repl{"master", "aaaa", 0, "", 0, "bbbb", 500};
    auto response = command::execute({"INFO", "replication"}, store, repl);
    EXPECT_NE(response.find("master_replid2:bbbb"), std::string::npos);
    EXPECT_NE(response.find("second_repl_offset:500"), std::string::npos);
}

TEST(Command, PsyncReturnsCurrentReplidNotReplid2) {
    Store store;
    ReplicationInfo repl{"master", "aaaa1111aaaa1111aaaa1111aaaa1111aaaa1111", 0, "", 0,
                         "bbbb2222bbbb2222bbbb2222bbbb2222bbbb2222", 500};
    auto result = command::execute({"PSYNC", "?", "-1"}, store, repl);
    // FULLRESYNC should use current replid, not replid2
    EXPECT_TRUE(result.find("+FULLRESYNC aaaa1111aaaa1111aaaa1111aaaa1111aaaa1111 0\r\n") == 0);
    EXPECT_EQ(result.find("bbbb2222"), std::string::npos);  // replid2 should NOT appear
}