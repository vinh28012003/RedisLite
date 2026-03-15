package failover

import (
	"log"
	"time"
)

// Monitor tracks node health and triggers failover on master failure.
type Monitor struct {
	config  Config
	nodes   map[string]*Node   // addr → node state
	fails   map[string]int     // addr → consecutive failure count
	master  string             // current master addr
}

// NewMonitor creates a monitor for the given config.
func NewMonitor(cfg Config) *Monitor {
	m := &Monitor{
		config: cfg,
		nodes:  make(map[string]*Node),
		fails:  make(map[string]int),
	}
	for _, addr := range cfg.Nodes {
		m.nodes[addr] = &Node{Addr: addr}
	}
	return m
}

// Run starts the health check loop. Blocks until stopped.
//
// TODO: Implement the main loop. This is the core of the controller.
//
// Each iteration:
//   1. PING all nodes
//   2. Update alive/role state
//   3. If master is dead (consecutive failures >= threshold):
//        a. Pick best replica (highest repl_offset)
//        b. Call promoter.Promote(bestReplica)
//        c. Call promoter.Reconfigure(siblings, newMaster)
//        d. Update internal state
//   4. Sleep PingInterval
//
// Consider: What happens if the controller itself restarts?
// It should discover the current topology via ROLE commands, not assume.
func (m *Monitor) Run() {
	log.Printf("Starting monitor: %d nodes, ping every %s, failure after %d misses",
		len(m.nodes), m.config.PingInterval, m.config.FailureThresh)

	// Discover initial topology
	m.discoverTopology()

	for {
		m.healthCheck()
		time.Sleep(m.config.PingInterval)
	}
}

// discoverTopology queries ROLE on all nodes to learn who is master/replica.
//
// TODO: Implement — send ROLE to each node, parse response, set m.master.
// This runs once at startup so the controller doesn't assume node roles.
func (m *Monitor) discoverTopology() {
	log.Println("Discovering topology...")
	// TODO
}

// healthCheck pings all nodes and triggers failover if master is dead.
//
// TODO: Implement the health check logic.
// For each node:
//   - Ping(addr, timeout) → alive
//   - If alive: reset fail counter, update role
//   - If dead: increment fail counter
//   - If master fail counter >= threshold: triggerFailover()
func (m *Monitor) healthCheck() {
	// TODO
}
