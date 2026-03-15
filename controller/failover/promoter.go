package failover

// Promoter handles the failover sequence:
//   1. Pick the best replica (highest replication offset)
//   2. Send REPLICAOF NO ONE to promote it
//   3. Reconfigure remaining replicas to follow the new master

// TODO: Implement these functions. This is where your design choices matter most.
//
// PickBestReplica: Query INFO replication on each replica, compare master_repl_offset.
//   - Highest offset = least data loss
//   - What if two replicas have the same offset? (tiebreak by addr for determinism)
//   - What if a replica is also unreachable? (skip it)
//
// Promote: Send REPLICAOF NO ONE to the chosen replica.
//   - Verify ROLE returns "master" after promotion
//   - What if promotion fails? (try next best replica)
//
// Reconfigure: Send REPLICAOF <new-master-host> <port> to remaining replicas.
//   - What if a sibling is unreachable? (retry later, don't block)
//   - Old master comes back? (reconfigure it as replica of new master)
