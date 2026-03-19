package failover

import (
	"log"
	"sync"
	"time"
)

// Monitor tracks node health and triggers failover on master failure.
type Monitor struct {
	config             Config
	nodes              map[string]*Node // addr → node state
	fails              map[string]int   // addr → consecutive failure count
	master             string           // current master addr
	failoverCount      int              // how many times triggerFailover was called
	failoverInProgress bool             // guard: prevents duplicate promotions
	roleFunc           func(addr string, timeout time.Duration) (RoleResult, error)
	pingFunc           func(addr string, timeout time.Duration) bool
	infoFunc           InfoFunc         // for PickBestReplica
	replicaOfFunc      ReplicaOfFunc    // for Promote + Reconfigure
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
	m.roleFunc = Role
	m.pingFunc = Ping
	m.infoFunc = Info
	m.replicaOfFunc = ReplicaOf
	return m
}

// Run starts the health check loop. Blocks until stopped.
//
// Each iteration:
//  1. PING all nodes
//  2. Update alive/role state
//  3. If master is dead (consecutive failures >= threshold):
//     a. Pick best replica (highest repl_offset)
//     b. Call promoter.Promote(bestReplica)
//     c. Call promoter.Reconfigure(siblings, newMaster)
//     d. Update internal state
//  4. Sleep PingInterval
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
// Runs once at startup. Unreachable nodes are marked dead — health loop picks them up.
func (m *Monitor) discoverTopology() {
	log.Println("Discovering topology...")

	masterCount := 0
	alive := 0

	for addr, node := range m.nodes {
		// Role() opens TCP, sends ROLE command, parses RESP response
		roleResult, err := m.roleFunc(addr, m.config.PingTimeout)

		if err != nil {
			// Decision B: skip unreachable nodes, don't crash
			node.Alive = false
			node.Role = "unknown"
			log.Printf(" [discovery] %s unreachable: %v", addr, err)
			continue
		}

		node.Alive = true
		node.Role = roleResult.Role
		alive++

		if roleResult.Role == "master" {
			m.master = addr
			masterCount++
			log.Printf(" [discovery] %s -> master (offset %d)", addr, roleResult.Offset)
		} else {
			log.Printf(" [discovery] %s -> %s", addr, roleResult.Role)
		}
	}

	// Post-loop diagnostics
	if masterCount == 0 {
		log.Println(" [discovery] WARNING: no master found, health loop will discover")
	}
	if masterCount > 1 {
		log.Printf(" [discovery] WARNING: %d masters detected, using %s", masterCount, m.master)
	}

	log.Printf("Topology discovered: master=%s, %d/%d nodes reachable", m.master, alive, len(m.nodes))
}

// pingResult carries one goroutine's PING outcome back to the main loop.
type pingResult struct {
	addr  string
	alive bool
}

// healthCheck pings all nodes concurrently and updates failure counters.
// If the master's consecutive failures reach FailureThresh, triggers failover.
func (m *Monitor) healthCheck() {
	// Fan-out: one goroutine per node, buffered channel collects results
	ch := make(chan pingResult, len(m.nodes))
	var wg sync.WaitGroup

	for addr := range m.nodes {
		wg.Add(1)
		go func(a string) {
			defer wg.Done()
			ch <- pingResult{a, m.pingFunc(a, m.config.PingTimeout)}
		}(addr)
	}

	wg.Wait()
	close(ch)

	// Process results sequentially — no lock needed, only this goroutine reads
	for r := range ch {
		node := m.nodes[r.addr]

		if r.alive {
			wasDead := !node.Alive
			m.fails[r.addr] = 0
			node.Alive = true

			// Dead→alive transition: re-query ROLE (role may have changed)
			if wasDead {
				roleResult, err := m.roleFunc(r.addr, m.config.PingTimeout)
				if err != nil {
					log.Printf(" [health] %s alive but ROLE query failed: %v", r.addr, err)
					continue
				}
				node.Role = roleResult.Role
				log.Printf(" [health] %s recovered, role=%s", r.addr, roleResult.Role)

				// Master role handling on recovery
				if roleResult.Role == "master" {
					if m.master == "" || m.master == r.addr {
						m.master = r.addr
						log.Printf(" [health] %s adopted as master", r.addr)
					} else {
						// Stale master recovered — reconfigure as replica of current master
						log.Printf(" [health] %s claims master but %s is master, reconfiguring", r.addr, m.master)
						errs := Reconfigure([]string{r.addr}, m.master, m.config.PingTimeout, m.replicaOfFunc)
						if errs[0] == nil {
							node.Role = "slave"
						}
					}
				}
			}

			// Alive-path: stale master that failed prior reconfigure — retry each tick
			if node.Role == "master" && r.addr != m.master && m.master != "" {
				log.Printf(" [health] %s still stale master, retrying reconfigure", r.addr)
				errs := Reconfigure([]string{r.addr}, m.master, m.config.PingTimeout, m.replicaOfFunc)
				if errs[0] == nil {
					node.Role = "slave"
				}
			}
			continue
		}

		// PING failed
		m.fails[r.addr]++
		if m.fails[r.addr] >= m.config.FailureThresh {
			if node.Alive {
				node.Alive = false
				log.Printf(" [health] %s is DEAD (failed %d/%d)", r.addr, m.fails[r.addr], m.config.FailureThresh)
			}
			if r.addr == m.master {
				m.triggerFailover()
			}
		} else {
			log.Printf(" [health] %s SUSPECT (fail %d/%d)", r.addr, m.fails[r.addr], m.config.FailureThresh)
		}
	}
}

// triggerFailover handles master death: select best replica, promote it, update state.
func (m *Monitor) triggerFailover() {
	if m.failoverInProgress {
		log.Println(" [failover] already in progress, skipping")
		return
	}
	m.failoverInProgress = true
	defer func() { m.failoverInProgress = false }()

	m.failoverCount++
	log.Printf("FAILOVER #%d: master %s is dead", m.failoverCount, m.master)

	// Build candidate list: alive replicas only
	var candidates []string
	for addr, node := range m.nodes {
		if node.Alive && node.Role == "slave" && addr != m.master {
			candidates = append(candidates, addr)
		}
	}
	if len(candidates) == 0 {
		log.Println(" [failover] no alive replicas available, will retry next cycle")
		return
	}

	// Stage 4: rank by replication offset
	ranked, err := PickBestReplica(candidates, m.config.PingTimeout, m.infoFunc)
	if err != nil {
		log.Printf(" [failover] replica selection failed: %v", err)
		return
	}

	// Stage 5: promote best candidate (with fallback through ranked list)
	newMaster, err := Promote(ranked, m.config.PingTimeout, m.replicaOfFunc, m.roleFunc)
	if err != nil {
		log.Printf(" [failover] promotion failed: %v", err)
		return
	}

	// Update internal state
	oldMaster := m.master
	m.master = newMaster
	m.nodes[newMaster].Role = "master"
	log.Printf("FAILOVER COMPLETE: %s → %s", oldMaster, newMaster)

	// Stage 6: reconfigure surviving siblings to follow new master
	var siblings []string
	for addr, node := range m.nodes {
		if addr != newMaster && addr != oldMaster && node.Alive {
			siblings = append(siblings, addr)
		}
	}
	if len(siblings) > 0 {
		Reconfigure(siblings, newMaster, m.config.PingTimeout, m.replicaOfFunc)
	}
}
