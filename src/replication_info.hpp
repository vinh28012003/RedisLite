#pragma once

#include <string>
#include <random>

// Generate a 40-character random hex string (replication ID).
// Uses std::random_device for OS entropy seeding mt19937.
inline std::string generate_replid() {
    static constexpr char hex[] = "0123456789abcdef";
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dist(0, 15);
    std::string id(40, '0');
    for (auto& c : id) c = hex[dist(gen)];
    return id;
}

// Replication metadata for this server instance.
// Constructed at startup from CLI flags (--replicaof determines role).
// Passed to command handler so INFO can report server identity.
struct ReplicationInfo {
    std::string role;  // "master" or "worker"
    std::string master_replid;
    int64_t master_repl_offset = 0;
    std::string master_host;   // Empty when master, set when replica
    int master_port = 0;       // 0 when master, set when replica
    std::string master_replid2 = std::string(40, '0');  // secondary ID (all zeros = no secondary)
    int64_t second_repl_offset = -1;                     // -1 = unused
};
