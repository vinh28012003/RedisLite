#pragma once
// Replication metadata for this server instance.
// Constructed at startup from CLI flags (--replicaof determines role).
// Passed to command handler so INFO can report server identity.
struct ReplicationInfo {
    std::string role;  // "master" or "replica"
    // Future stages will add:
    // std::string master_replid;
    // int64_t master_repl_offset;
    // int connected_slaves;
};