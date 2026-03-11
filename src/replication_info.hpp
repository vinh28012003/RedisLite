#pragma once
// Replication metadata for this server instance.
// Constructed at startup from CLI flags (--replicaof determines role).
// Passed to command handler so INFO can report server identity.
struct ReplicationInfo {
    std::string role;  // "master" or "worker"
    std::string master_replid;
    int64_t master_repl_offset = 0;
    std::string master_host;   // Empty when master, set when replica
    int master_port = 0;       // 0 when master, set when replica
};