package failover

import "time"

// Node represents a single redis-lite instance the controller monitors.
type Node struct {
	Addr string // "redis-1:6379"
	Role string // "master" or "worker" (updated by health checks)
	Alive bool
}

// Config holds controller settings — tuned for <5s failover.
type Config struct {
	Nodes         []string      // addresses: ["redis-1:6379", "redis-2:6379", "redis-3:6379"]
	PingInterval  time.Duration // how often to PING each node (default 500ms)
	PingTimeout   time.Duration // PING response deadline (default 500ms)
	FailureThresh int           // consecutive PING failures before declaring dead (default 3)
	// FailureThresh * PingInterval = detection time (3 * 500ms = 1.5s)
}

// DefaultConfig returns production defaults for <5s failover.
func DefaultConfig(nodes []string) Config {
	return Config{
		Nodes:         nodes,
		PingInterval:  500 * time.Millisecond,
		PingTimeout:   500 * time.Millisecond,
		FailureThresh: 3,
	}
}

// Node discovery: currently Option A (static REDIS_NODES env var).
// For EKS production, switch to Option C (DNS-based discovery):
//
//   type NodeDiscovery interface {
//       Discover() []string
//   }
//   type StaticDiscovery struct { nodes []string }                // Option A (current)
//   type DNSDiscovery struct { service, namespace string }        // Option C (EKS)
//
// DNSDiscovery does SRV lookup on a Kubernetes headless Service.
// New StatefulSet replicas appear in DNS automatically zero manual config.

