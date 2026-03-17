//go:build unit

package failover

import (
	"fmt"
	"testing"
	"time"
)

// mockRoleFunc builds a roleFunc from a map of addr → RoleResult.
// Addrs not in the map return an error (simulates unreachable node).
func mockRoleFunc(roles map[string]RoleResult) func(string, time.Duration) (RoleResult, error) {
	return func(addr string, timeout time.Duration) (RoleResult, error) {
		if r, ok := roles[addr]; ok {
				return r, nil
		}
		return RoleResult{}, fmt.Errorf("connect %s: connection refused", addr)
	}
}

// mockPingFunc builds a pingFunc from a set of alive addrs.
// Addrs in the set return true; others return false.
func mockPingFunc(alive map[string]bool) func(string, time.Duration) bool {
	return func(addr string, timeout time.Duration) bool {
		return alive[addr]
	}
}

// helper: create a Monitor with mockRoleFunc wired in
func newTestMonitor(addrs []string, roles map[string]RoleResult) *Monitor {
	m := NewMonitor(DefaultConfig(addrs))
	m.roleFunc = mockRoleFunc(roles)
	return m
}

// helper: create a Monitor with both mock seams, topology pre-discovered
func newHealthTestMonitor(addrs []string, masterAddr string, pings map[string]bool, roles map[string]RoleResult) *Monitor {
	m := NewMonitor(DefaultConfig(addrs))
	m.roleFunc = mockRoleFunc(roles)
	m.pingFunc = mockPingFunc(pings)
	// Pre-set topology as if discoverTopology already ran
	m.master = masterAddr
	for _, addr := range addrs {
		m.nodes[addr].Alive = true
		if addr == masterAddr {
			m.nodes[addr].Role = "master"
		} else {
			m.nodes[addr].Role = "slave"
		}
	}
	return m
}

func TestDiscoverTopology_HappyPath(t *testing.T) {
	// 1 master + 2 replicas — normal startup
	addrs := []string{"redis-1:6379", "redis-2:6379", "redis-3:6379"}
	roles := map[string]RoleResult{
			"redis-1:6379": {Role: "master", Offset: 100},
			"redis-2:6379": {Role: "slave", Offset: 100, MasterHost: "redis-1", MasterPort: "6379"},
			"redis-3:6379": {Role: "slave", Offset: 80, MasterHost: "redis-1", MasterPort: "6379"},
	}

	m := newTestMonitor(addrs, roles)
	m.discoverTopology()

	if m.master != "redis-1:6379" {
			t.Errorf("master = %q, want redis-1:6379", m.master)
	}
	for _, addr := range addrs {
			node := m.nodes[addr]
			if !node.Alive {
					t.Errorf("%s should be alive", addr)
			}
	}
	if m.nodes["redis-1:6379"].Role != "master" {
			t.Errorf("redis-1 role = %q, want master", m.nodes["redis-1:6379"].Role)
	}
	if m.nodes["redis-2:6379"].Role != "slave" {
			t.Errorf("redis-2 role = %q, want slave", m.nodes["redis-2:6379"].Role)
	}
}

func TestDiscoverTopology_AllUnreachable(t *testing.T) {
	// All nodes down — controller starts before redis boots
	addrs := []string{"redis-1:6379", "redis-2:6379"}
	roles := map[string]RoleResult{} // empty → all unreachable

	m := newTestMonitor(addrs, roles)
	m.discoverTopology()

	if m.master != "" {
			t.Errorf("master = %q, want empty", m.master)
	}
	for _, addr := range addrs {
			node := m.nodes[addr]
			if node.Alive {
					t.Errorf("%s should be dead", addr)
			}
			if node.Role != "unknown" {
					t.Errorf("%s role = %q, want unknown", addr, node.Role)
			}
	}
}

func TestDiscoverTopology_MixedReachability(t *testing.T) {
	// Master reachable, one replica down
	addrs := []string{"redis-1:6379", "redis-2:6379", "redis-3:6379"}
	roles := map[string]RoleResult{
			"redis-1:6379": {Role: "master", Offset: 50},
			"redis-3:6379": {Role: "slave", Offset: 50, MasterHost: "redis-1", MasterPort: "6379"},
			// redis-2 missing from map → unreachable
	}

	m := newTestMonitor(addrs, roles)
	m.discoverTopology()

	if m.master != "redis-1:6379" {
			t.Errorf("master = %q, want redis-1:6379", m.master)
	}
	if m.nodes["redis-2:6379"].Alive {
			t.Errorf("redis-2 should be dead")
	}
	if m.nodes["redis-2:6379"].Role != "unknown" {
			t.Errorf("redis-2 role = %q, want unknown", m.nodes["redis-2:6379"].Role)
	}
	if !m.nodes["redis-3:6379"].Alive {
			t.Errorf("redis-3 should be alive")
	}
}

func TestDiscoverTopology_NoMaster(t *testing.T) {
	// All replicas, no master — possible if master died before controller started
	addrs := []string{"redis-1:6379", "redis-2:6379"}
	roles := map[string]RoleResult{
			"redis-1:6379": {Role: "slave", Offset: 100},
			"redis-2:6379": {Role: "slave", Offset: 80},
	}

	m := newTestMonitor(addrs, roles)
	m.discoverTopology()

	if m.master != "" {
			t.Errorf("master = %q, want empty (no master in cluster)", m.master)
	}
	for _, addr := range addrs {
			if !m.nodes[addr].Alive {
					t.Errorf("%s should be alive", addr)
			}
	}
}

func TestDiscoverTopology_MultipleMasters(t *testing.T) {
	// Split-brain: two nodes claim master — old master came back
	addrs := []string{"redis-1:6379", "redis-2:6379", "redis-3:6379"}
	roles := map[string]RoleResult{
			"redis-1:6379": {Role: "master", Offset: 100},
			"redis-2:6379": {Role: "master", Offset: 200},
			"redis-3:6379": {Role: "slave", Offset: 150},
	}

	m := newTestMonitor(addrs, roles)
	m.discoverTopology()

	// m.master should be one of the two masters (map iteration order is random)
	if m.master != "redis-1:6379" && m.master != "redis-2:6379" {
			t.Errorf("master = %q, want redis-1:6379 or redis-2:6379", m.master)
	}
	// Both masters should be alive
	if !m.nodes["redis-1:6379"].Alive || !m.nodes["redis-2:6379"].Alive {
			t.Error("both masters should be alive")
	}
}

// --- Health Check Tests ---

func TestHealthCheck_AllHealthy(t *testing.T) {
	// All nodes respond to PING — no failures, no failover
	addrs := []string{"redis-1:6379", "redis-2:6379", "redis-3:6379"}
	pings := map[string]bool{
		"redis-1:6379": true,
		"redis-2:6379": true,
		"redis-3:6379": true,
	}

	m := newHealthTestMonitor(addrs, "redis-1:6379", pings, nil)
	m.healthCheck()

	for _, addr := range addrs {
		if m.fails[addr] != 0 {
			t.Errorf("%s fails = %d, want 0", addr, m.fails[addr])
		}
		if !m.nodes[addr].Alive {
			t.Errorf("%s should be alive", addr)
		}
	}
	if m.failoverCount != 0 {
		t.Errorf("failoverCount = %d, want 0", m.failoverCount)
	}
}

func TestHealthCheck_ReplicaFailure_NoFailover(t *testing.T) {
	// One replica dies — marked DEAD after 3 failures, but no failover
	addrs := []string{"redis-1:6379", "redis-2:6379", "redis-3:6379"}
	pings := map[string]bool{
		"redis-1:6379": true,  // master alive
		"redis-2:6379": false, // replica dead
		"redis-3:6379": true,  // replica alive
	}

	m := newHealthTestMonitor(addrs, "redis-1:6379", pings, nil)

	// 3 health checks to cross FailureThresh
	m.healthCheck()
	m.healthCheck()
	m.healthCheck()

	if m.nodes["redis-2:6379"].Alive {
		t.Error("redis-2 should be dead")
	}
	if m.fails["redis-2:6379"] != 3 {
		t.Errorf("redis-2 fails = %d, want 3", m.fails["redis-2:6379"])
	}
	if m.failoverCount != 0 {
		t.Errorf("failoverCount = %d, want 0 (replica death doesn't trigger failover)", m.failoverCount)
	}
}

func TestHealthCheck_MasterFailure_TriggersFailover(t *testing.T) {
	// Master dies — after 3 consecutive failures, triggerFailover is called
	addrs := []string{"redis-1:6379", "redis-2:6379", "redis-3:6379"}
	pings := map[string]bool{
		"redis-1:6379": false, // master dead
		"redis-2:6379": true,
		"redis-3:6379": true,
	}

	m := newHealthTestMonitor(addrs, "redis-1:6379", pings, nil)

	m.healthCheck() // fail 1 — SUSPECT
	m.healthCheck() // fail 2 — SUSPECT
	if m.failoverCount != 0 {
		t.Errorf("failover triggered too early: count = %d", m.failoverCount)
	}

	m.healthCheck() // fail 3 — DEAD → failover

	if m.failoverCount != 1 {
		t.Errorf("failoverCount = %d, want 1", m.failoverCount)
	}
	if m.nodes["redis-1:6379"].Alive {
		t.Error("master should be dead")
	}
}

func TestHealthCheck_SuspectState(t *testing.T) {
	// Master fails 1-2 times — still alive (SUSPECT), no failover
	addrs := []string{"redis-1:6379", "redis-2:6379"}
	pings := map[string]bool{
		"redis-1:6379": false, // master failing
		"redis-2:6379": true,
	}

	m := newHealthTestMonitor(addrs, "redis-1:6379", pings, nil)

	m.healthCheck() // fail 1
	if m.fails["redis-1:6379"] != 1 {
		t.Errorf("fails = %d, want 1", m.fails["redis-1:6379"])
	}
	if !m.nodes["redis-1:6379"].Alive {
		t.Error("master should still be alive (SUSPECT, not DEAD)")
	}

	m.healthCheck() // fail 2
	if m.fails["redis-1:6379"] != 2 {
		t.Errorf("fails = %d, want 2", m.fails["redis-1:6379"])
	}
	if !m.nodes["redis-1:6379"].Alive {
		t.Error("master should still be alive (SUSPECT, not DEAD)")
	}
	if m.failoverCount != 0 {
		t.Errorf("failoverCount = %d, want 0", m.failoverCount)
	}
}

func TestHealthCheck_RecoveryResetsCounter(t *testing.T) {
	// Master fails twice (SUSPECT), then recovers — counter resets
	addrs := []string{"redis-1:6379", "redis-2:6379"}
	failingPings := map[string]bool{
		"redis-1:6379": false,
		"redis-2:6379": true,
	}
	healthyPings := map[string]bool{
		"redis-1:6379": true,
		"redis-2:6379": true,
	}

	m := newHealthTestMonitor(addrs, "redis-1:6379", failingPings, nil)

	// Fail twice
	m.healthCheck()
	m.healthCheck()
	if m.fails["redis-1:6379"] != 2 {
		t.Errorf("fails = %d, want 2", m.fails["redis-1:6379"])
	}

	// Recover — switch to healthy pings
	m.pingFunc = mockPingFunc(healthyPings)
	m.healthCheck()

	if m.fails["redis-1:6379"] != 0 {
		t.Errorf("fails = %d, want 0 (should reset on recovery)", m.fails["redis-1:6379"])
	}
	if !m.nodes["redis-1:6379"].Alive {
		t.Error("master should be alive after recovery")
	}
	if m.failoverCount != 0 {
		t.Errorf("failoverCount = %d, want 0", m.failoverCount)
	}
}

func TestHealthCheck_DeadNodeRecovers_RoleRequeried(t *testing.T) {
	// A replica dies, then comes back — ROLE is re-queried on recovery
	addrs := []string{"redis-1:6379", "redis-2:6379", "redis-3:6379"}
	deadPings := map[string]bool{
		"redis-1:6379": true,
		"redis-2:6379": false, // replica dies
		"redis-3:6379": true,
	}
	roles := map[string]RoleResult{
		"redis-2:6379": {Role: "slave", Offset: 200},
	}

	m := newHealthTestMonitor(addrs, "redis-1:6379", deadPings, roles)

	// Kill replica for 3 ticks → DEAD
	m.healthCheck()
	m.healthCheck()
	m.healthCheck()
	if m.nodes["redis-2:6379"].Alive {
		t.Error("redis-2 should be dead")
	}

	// Bring it back — switch to alive pings
	alivePings := map[string]bool{
		"redis-1:6379": true,
		"redis-2:6379": true, // recovered
		"redis-3:6379": true,
	}
	m.pingFunc = mockPingFunc(alivePings)
	m.healthCheck()

	if !m.nodes["redis-2:6379"].Alive {
		t.Error("redis-2 should be alive after recovery")
	}
	if m.nodes["redis-2:6379"].Role != "slave" {
		t.Errorf("redis-2 role = %q, want slave (should be re-queried on recovery)", m.nodes["redis-2:6379"].Role)
	}
	if m.fails["redis-2:6379"] != 0 {
		t.Errorf("fails = %d, want 0", m.fails["redis-2:6379"])
	}
}

// --- Edge Case Tests ---

func TestHealthCheck_RepeatedFailoverTrigger(t *testing.T) {
	// Master stays dead past threshold — triggerFailover called every tick
	// Documents current behavior; Stage 5 adds failoverInProgress guard
	addrs := []string{"redis-1:6379", "redis-2:6379"}
	pings := map[string]bool{
		"redis-1:6379": false, // master stays dead
		"redis-2:6379": true,
	}

	m := newHealthTestMonitor(addrs, "redis-1:6379", pings, nil)

	// Ticks 1-3: reach threshold
	m.healthCheck()
	m.healthCheck()
	m.healthCheck()
	if m.failoverCount != 1 {
		t.Errorf("failoverCount = %d, want 1 after first threshold", m.failoverCount)
	}

	// Ticks 4-5: master still dead — failover fires again each tick
	m.healthCheck()
	m.healthCheck()
	if m.failoverCount != 3 {
		t.Errorf("failoverCount = %d, want 3 (fires every tick while master is dead)", m.failoverCount)
	}
}

func TestHealthCheck_RoleQueryFailsOnRecovery(t *testing.T) {
	// Node recovers (PING ok) but ROLE query fails — node alive with stale role
	addrs := []string{"redis-1:6379", "redis-2:6379"}
	deadPings := map[string]bool{
		"redis-1:6379": true,
		"redis-2:6379": false,
	}
	// Empty roles map → ROLE query returns error for all addrs
	roles := map[string]RoleResult{}

	m := newHealthTestMonitor(addrs, "redis-1:6379", deadPings, roles)

	// Kill redis-2 for 3 ticks
	m.healthCheck()
	m.healthCheck()
	m.healthCheck()
	if m.nodes["redis-2:6379"].Alive {
		t.Error("redis-2 should be dead")
	}

	// Bring it back — PING succeeds, but ROLE will fail
	alivePings := map[string]bool{
		"redis-1:6379": true,
		"redis-2:6379": true,
	}
	m.pingFunc = mockPingFunc(alivePings)
	m.healthCheck()

	// Node is alive but role stays stale (whatever it was before)
	if !m.nodes["redis-2:6379"].Alive {
		t.Error("redis-2 should be alive (PING succeeded)")
	}
	// Role should be "slave" — unchanged from initial setup in newHealthTestMonitor
	if m.nodes["redis-2:6379"].Role != "slave" {
		t.Errorf("redis-2 role = %q, want slave (stale, ROLE query failed)", m.nodes["redis-2:6379"].Role)
	}
}

func TestHealthCheck_BootRaceRecovery(t *testing.T) {
	// Node was unreachable at startup (discoverTopology marked it dead/unknown)
	// Health check later finds it alive — ROLE re-queried, state updated
	addrs := []string{"redis-1:6379", "redis-2:6379"}
	roles := map[string]RoleResult{
		"redis-2:6379": {Role: "slave", Offset: 50},
	}

	m := NewMonitor(DefaultConfig(addrs))
	m.roleFunc = mockRoleFunc(roles)

	// Simulate discoverTopology finding only redis-2
	// redis-1 stays at zero value: Alive=false, Role=""
	m.nodes["redis-2:6379"].Alive = true
	m.nodes["redis-2:6379"].Role = "slave"
	// redis-1 is dead/unknown (boot race — started after controller)

	// Now redis-1 comes alive as master
	pings := map[string]bool{
		"redis-1:6379": true,
		"redis-2:6379": true,
	}
	m.pingFunc = mockPingFunc(pings)
	m.roleFunc = mockRoleFunc(map[string]RoleResult{
		"redis-1:6379": {Role: "master", Offset: 100},
		"redis-2:6379": {Role: "slave", Offset: 50},
	})

	m.healthCheck()

	// redis-1 should be alive with role=master, adopted as master
	if !m.nodes["redis-1:6379"].Alive {
		t.Error("redis-1 should be alive")
	}
	if m.nodes["redis-1:6379"].Role != "master" {
		t.Errorf("redis-1 role = %q, want master", m.nodes["redis-1:6379"].Role)
	}
	if m.master != "redis-1:6379" {
		t.Errorf("master = %q, want redis-1:6379 (should be adopted)", m.master)
	}
}

func TestHealthCheck_NoMasterDiscovered_NodeClaimsMaster(t *testing.T) {
	// discoverTopology found no master (m.master == "")
	// A node recovers claiming master — should be adopted as master (bug #4 fix)
	addrs := []string{"redis-1:6379", "redis-2:6379"}

	m := NewMonitor(DefaultConfig(addrs))
	// Simulate: no master discovered, all nodes were dead at startup
	m.master = ""
	m.nodes["redis-1:6379"].Alive = false
	m.nodes["redis-1:6379"].Role = "unknown"
	m.nodes["redis-2:6379"].Alive = false
	m.nodes["redis-2:6379"].Role = "unknown"

	// Now redis-1 comes alive as master
	pings := map[string]bool{
		"redis-1:6379": true,
		"redis-2:6379": false,
	}
	roles := map[string]RoleResult{
		"redis-1:6379": {Role: "master", Offset: 100},
	}
	m.pingFunc = mockPingFunc(pings)
	m.roleFunc = mockRoleFunc(roles)

	m.healthCheck()

	if m.master != "redis-1:6379" {
		t.Errorf("master = %q, want redis-1:6379 (should adopt when no master known)", m.master)
	}
	if !m.nodes["redis-1:6379"].Alive {
		t.Error("redis-1 should be alive")
	}
	if m.nodes["redis-1:6379"].Role != "master" {
		t.Errorf("redis-1 role = %q, want master", m.nodes["redis-1:6379"].Role)
	}
}
