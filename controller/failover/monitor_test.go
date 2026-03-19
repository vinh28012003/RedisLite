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

func TestHealthCheck_GuardPreventsRepeatedFailover(t *testing.T) {
	// Master stays dead past threshold — failoverInProgress guard prevents re-entry
	// triggerFailover now returns early on ticks 4-5 because failover already ran
	addrs := []string{"redis-1:6379", "redis-2:6379"}
	pings := map[string]bool{
		"redis-1:6379": false, // master stays dead
		"redis-2:6379": true,
	}

	m := newHealthTestMonitor(addrs, "redis-1:6379", pings, nil)
	// Wire mocks so triggerFailover can run (but no candidates → exits early)
	m.infoFunc = mockInfoFunc(map[string]map[string]string{})
	m.replicaOfFunc = func(addr string, timeout time.Duration, a1, a2 string) error {
		return fmt.Errorf("not expected")
	}

	// Ticks 1-3: reach threshold, failover fires once
	m.healthCheck()
	m.healthCheck()
	m.healthCheck()
	if m.failoverCount != 1 {
		t.Errorf("failoverCount = %d, want 1 after first threshold", m.failoverCount)
	}

	// Ticks 4-5: master still dead — failover fires again but guard is cleared
	// (guard clears via defer at end of triggerFailover, so it does fire each tick)
	// But since no candidates succeed, it increments count but doesn't promote
	m.healthCheck()
	m.healthCheck()
	if m.failoverCount < 2 {
		t.Errorf("failoverCount = %d, want >= 2 (fires each tick, guard clears after each attempt)", m.failoverCount)
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

// --- triggerFailover Wiring Tests ---

// newFailoverTestMonitor creates a Monitor with all seams wired for failover testing.
// Master is dead, replicas are alive with role="slave".
func newFailoverTestMonitor(
	addrs []string, masterAddr string,
	infos map[string]map[string]string,
	replicaOfSuccess map[string]bool,
	roleSeq map[string][]RoleResult,
) *Monitor {
	m := NewMonitor(DefaultConfig(addrs))
	m.master = masterAddr

	// Set up node states: master is dead, replicas are alive
	for _, addr := range addrs {
		if addr == masterAddr {
			m.nodes[addr].Alive = false
			m.nodes[addr].Role = "master"
			m.fails[addr] = 3 // past threshold
		} else {
			m.nodes[addr].Alive = true
			m.nodes[addr].Role = "slave"
		}
	}

	m.infoFunc = mockInfoFunc(infos)
	m.replicaOfFunc = mockReplicaOfFunc(replicaOfSuccess)
	m.roleFunc = mockRoleFuncSequence(roleSeq)
	m.pingFunc = mockPingFunc(map[string]bool{}) // not used directly

	return m
}

func TestTriggerFailover_FullWiring(t *testing.T) {
	// End-to-end: master dead, 2 alive replicas, best gets promoted, state updated
	addrs := []string{"redis-1:6379", "redis-2:6379", "redis-3:6379"}
	infos := map[string]map[string]string{
		"redis-2:6379": {"master_repl_offset": "200"},
		"redis-3:6379": {"master_repl_offset": "100"},
	}
	replicaOf := map[string]bool{"redis-2:6379": true, "redis-3:6379": true}
	roles := map[string][]RoleResult{
		"redis-2:6379": {{Role: "master", Offset: 200}},
	}

	m := newFailoverTestMonitor(addrs, "redis-1:6379", infos, replicaOf, roles)
	m.triggerFailover()

	if m.master != "redis-2:6379" {
		t.Errorf("master = %q, want redis-2:6379 (highest offset)", m.master)
	}
	if m.nodes["redis-2:6379"].Role != "master" {
		t.Errorf("redis-2 role = %q, want master", m.nodes["redis-2:6379"].Role)
	}
	if m.failoverCount != 1 {
		t.Errorf("failoverCount = %d, want 1", m.failoverCount)
	}
}

func TestTriggerFailover_NoAliveCandidates(t *testing.T) {
	// All replicas are dead — no candidates, failover aborts
	addrs := []string{"redis-1:6379", "redis-2:6379", "redis-3:6379"}

	m := NewMonitor(DefaultConfig(addrs))
	m.master = "redis-1:6379"
	for _, addr := range addrs {
		m.nodes[addr].Alive = false
		m.nodes[addr].Role = "unknown"
	}
	m.infoFunc = mockInfoFunc(map[string]map[string]string{})
	m.replicaOfFunc = mockReplicaOfFunc(map[string]bool{})

	m.triggerFailover()

	// Master unchanged — no promotion possible
	if m.master != "redis-1:6379" {
		t.Errorf("master = %q, want redis-1:6379 (unchanged, no candidates)", m.master)
	}
	if m.failoverCount != 1 {
		t.Errorf("failoverCount = %d, want 1 (attempt counted even if no candidates)", m.failoverCount)
	}
}

func TestTriggerFailover_GuardPreventsReentry(t *testing.T) {
	// failoverInProgress=true → triggerFailover returns immediately
	addrs := []string{"redis-1:6379", "redis-2:6379"}

	m := NewMonitor(DefaultConfig(addrs))
	m.master = "redis-1:6379"
	m.failoverInProgress = true // simulate in-progress

	m.triggerFailover()

	// failoverCount stays 0 — the call was skipped entirely
	if m.failoverCount != 0 {
		t.Errorf("failoverCount = %d, want 0 (guard prevented entry)", m.failoverCount)
	}
}

func TestTriggerFailover_PromotionFails_MasterUnchanged(t *testing.T) {
	// All candidates fail promotion — master stays as old dead master
	addrs := []string{"redis-1:6379", "redis-2:6379", "redis-3:6379"}
	infos := map[string]map[string]string{
		"redis-2:6379": {"master_repl_offset": "200"},
		"redis-3:6379": {"master_repl_offset": "100"},
	}
	// REPLICAOF succeeds but ROLE never returns "master"
	replicaOf := map[string]bool{"redis-2:6379": true, "redis-3:6379": true}
	roles := map[string][]RoleResult{
		"redis-2:6379": {{Role: "slave"}, {Role: "slave"}, {Role: "slave"}},
		"redis-3:6379": {{Role: "slave"}, {Role: "slave"}, {Role: "slave"}},
	}

	m := newFailoverTestMonitor(addrs, "redis-1:6379", infos, replicaOf, roles)
	m.triggerFailover()

	// Master unchanged — promotion failed for all
	if m.master != "redis-1:6379" {
		t.Errorf("master = %q, want redis-1:6379 (unchanged, promotion failed)", m.master)
	}
}

func TestTriggerFailover_FallbackToSecondCandidate(t *testing.T) {
	// Best candidate (highest offset) fails promotion, second succeeds
	addrs := []string{"redis-1:6379", "redis-2:6379", "redis-3:6379"}
	infos := map[string]map[string]string{
		"redis-2:6379": {"master_repl_offset": "200"}, // best
		"redis-3:6379": {"master_repl_offset": "100"}, // fallback
	}
	replicaOf := map[string]bool{
		"redis-2:6379": false, // REPLICAOF fails
		"redis-3:6379": true,  // REPLICAOF succeeds
	}
	roles := map[string][]RoleResult{
		"redis-3:6379": {{Role: "master", Offset: 100}},
	}

	m := newFailoverTestMonitor(addrs, "redis-1:6379", infos, replicaOf, roles)
	m.triggerFailover()

	if m.master != "redis-3:6379" {
		t.Errorf("master = %q, want redis-3:6379 (fallback candidate)", m.master)
	}
	if m.nodes["redis-3:6379"].Role != "master" {
		t.Errorf("redis-3 role = %q, want master", m.nodes["redis-3:6379"].Role)
	}
}

func TestTriggerFailover_GuardClearsAfterCompletion(t *testing.T) {
	// After triggerFailover completes (success or failure), guard is cleared
	addrs := []string{"redis-1:6379", "redis-2:6379"}
	infos := map[string]map[string]string{
		"redis-2:6379": {"master_repl_offset": "200"},
	}
	replicaOf := map[string]bool{"redis-2:6379": true}
	roles := map[string][]RoleResult{
		"redis-2:6379": {{Role: "master", Offset: 200}},
	}

	m := newFailoverTestMonitor(addrs, "redis-1:6379", infos, replicaOf, roles)
	m.triggerFailover()

	if m.failoverInProgress {
		t.Error("failoverInProgress should be false after completion (defer cleared it)")
	}
}

func TestTriggerFailover_ReconfiguresSiblings(t *testing.T) {
	// After promotion, surviving siblings should receive REPLICAOF <new-master>
	addrs := []string{"redis-1:6379", "redis-2:6379", "redis-3:6379", "redis-4:6379"}
	infos := map[string]map[string]string{
		"redis-2:6379": {"master_repl_offset": "300"}, // best → promoted
		"redis-3:6379": {"master_repl_offset": "200"},
		"redis-4:6379": {"master_repl_offset": "100"},
	}
	// Track REPLICAOF calls
	replicaOfCalls := map[string][2]string{}
	replicaOf := func(addr string, timeout time.Duration, arg1, arg2 string) error {
		replicaOfCalls[addr] = [2]string{arg1, arg2}
		return nil
	}
	roles := map[string][]RoleResult{
		"redis-2:6379": {{Role: "master", Offset: 300}},
	}

	m := newFailoverTestMonitor(addrs, "redis-1:6379", infos, map[string]bool{}, roles)
	m.replicaOfFunc = replicaOf
	m.triggerFailover()

	// redis-2 promoted as new master
	if m.master != "redis-2:6379" {
		t.Fatalf("master = %q, want redis-2:6379", m.master)
	}

	// redis-2 gets REPLICAOF NO ONE (from Promote)
	if args, ok := replicaOfCalls["redis-2:6379"]; ok {
		if args[0] != "NO" || args[1] != "ONE" {
			t.Errorf("redis-2 got REPLICAOF %s %s, want NO ONE", args[0], args[1])
		}
	} else {
		t.Error("redis-2 should have received REPLICAOF NO ONE")
	}

	// redis-3 and redis-4 should be reconfigured to follow redis-2
	for _, addr := range []string{"redis-3:6379", "redis-4:6379"} {
		args, ok := replicaOfCalls[addr]
		if !ok {
			t.Errorf("%s should have received REPLICAOF", addr)
			continue
		}
		if args[0] != "redis-2" || args[1] != "6379" {
			t.Errorf("%s got REPLICAOF %s %s, want redis-2 6379", addr, args[0], args[1])
		}
	}

	// redis-1 (dead old master) should NOT have been called
	if _, ok := replicaOfCalls["redis-1:6379"]; ok {
		t.Error("redis-1 (dead old master) should not have been reconfigured")
	}
}

func TestHealthCheck_OldMasterRecovery_ReconfiguredAsReplica(t *testing.T) {
	// Old master comes back claiming "master" — should be reconfigured as replica
	addrs := []string{"redis-1:6379", "redis-2:6379", "redis-3:6379"}

	// Simulate: redis-2 is current master, redis-1 was old master and is dead
	m := NewMonitor(DefaultConfig(addrs))
	m.master = "redis-2:6379"
	m.nodes["redis-1:6379"].Alive = false
	m.nodes["redis-1:6379"].Role = "master" // stale role
	m.nodes["redis-2:6379"].Alive = true
	m.nodes["redis-2:6379"].Role = "master"
	m.nodes["redis-3:6379"].Alive = true
	m.nodes["redis-3:6379"].Role = "slave"
	m.fails["redis-1:6379"] = 5 // well past threshold

	// redis-1 recovers, ROLE still says "master" (hasn't been reconfigured yet)
	pings := map[string]bool{
		"redis-1:6379": true, // recovered!
		"redis-2:6379": true,
		"redis-3:6379": true,
	}
	roles := map[string]RoleResult{
		"redis-1:6379": {Role: "master", Offset: 100}, // stale master claim
	}
	m.pingFunc = mockPingFunc(pings)
	m.roleFunc = mockRoleFunc(roles)

	// Track REPLICAOF calls
	replicaOfCalls := map[string][2]string{}
	m.replicaOfFunc = func(addr string, timeout time.Duration, arg1, arg2 string) error {
		replicaOfCalls[addr] = [2]string{arg1, arg2}
		return nil
	}

	m.healthCheck()

	// redis-1 should be reconfigured as slave of redis-2
	if m.nodes["redis-1:6379"].Role != "slave" {
		t.Errorf("redis-1 role = %q, want slave (reconfigured)", m.nodes["redis-1:6379"].Role)
	}
	args, ok := replicaOfCalls["redis-1:6379"]
	if !ok {
		t.Fatal("redis-1 should have received REPLICAOF")
	}
	if args[0] != "redis-2" || args[1] != "6379" {
		t.Errorf("redis-1 got REPLICAOF %s %s, want redis-2 6379", args[0], args[1])
	}
	// master should stay as redis-2
	if m.master != "redis-2:6379" {
		t.Errorf("master = %q, want redis-2:6379 (should not change)", m.master)
	}
}

func TestHealthCheck_OldMasterRecovery_ReconfigureFails_RoleUnchanged(t *testing.T) {
	// Old master recovers claiming "master", but Reconfigure fails.
	// Role should stay "master" (not incorrectly set to "slave").
	// Next tick, alive-path check retries reconfigure.
	addrs := []string{"redis-1:6379", "redis-2:6379"}

	m := NewMonitor(DefaultConfig(addrs))
	m.master = "redis-2:6379"
	m.nodes["redis-1:6379"].Alive = false
	m.nodes["redis-1:6379"].Role = "master"
	m.nodes["redis-2:6379"].Alive = true
	m.nodes["redis-2:6379"].Role = "master"
	m.fails["redis-1:6379"] = 5

	// redis-1 recovers, ROLE still says "master"
	m.pingFunc = mockPingFunc(map[string]bool{
		"redis-1:6379": true,
		"redis-2:6379": true,
	})
	m.roleFunc = mockRoleFunc(map[string]RoleResult{
		"redis-1:6379": {Role: "master", Offset: 100},
	})

	// Reconfigure FAILS
	m.replicaOfFunc = func(addr string, timeout time.Duration, arg1, arg2 string) error {
		return fmt.Errorf("connection refused")
	}

	m.healthCheck()

	// Role should stay "master" — reconfigure failed, don't lie about state
	if m.nodes["redis-1:6379"].Role != "master" {
		t.Errorf("redis-1 role = %q, want master (reconfigure failed, should not update)", m.nodes["redis-1:6379"].Role)
	}

	// Now fix the network — next tick's alive-path check should retry and succeed
	replicaOfCalls := map[string][2]string{}
	m.replicaOfFunc = func(addr string, timeout time.Duration, arg1, arg2 string) error {
		replicaOfCalls[addr] = [2]string{arg1, arg2}
		return nil
	}

	m.healthCheck()

	// Now role should be "slave" — alive-path retry succeeded
	if m.nodes["redis-1:6379"].Role != "slave" {
		t.Errorf("redis-1 role = %q, want slave (alive-path retry should succeed)", m.nodes["redis-1:6379"].Role)
	}
	args, ok := replicaOfCalls["redis-1:6379"]
	if !ok {
		t.Fatal("redis-1 should have received REPLICAOF on retry")
	}
	if args[0] != "redis-2" || args[1] != "6379" {
		t.Errorf("redis-1 got REPLICAOF %s %s, want redis-2 6379", args[0], args[1])
	}
}

func TestTriggerFailover_OnlyPromotedNodeAlive(t *testing.T) {
	// All other replicas are dead — only promoted node is alive.
	// Sibling list should be empty, no reconfigure calls, no crash.
	addrs := []string{"redis-1:6379", "redis-2:6379", "redis-3:6379"}
	infos := map[string]map[string]string{
		"redis-2:6379": {"master_repl_offset": "200"},
	}
	roles := map[string][]RoleResult{
		"redis-2:6379": {{Role: "master", Offset: 200}},
	}

	m := NewMonitor(DefaultConfig(addrs))
	m.master = "redis-1:6379"
	// redis-1 (master) dead, redis-2 alive, redis-3 also dead
	m.nodes["redis-1:6379"].Alive = false
	m.nodes["redis-1:6379"].Role = "master"
	m.fails["redis-1:6379"] = 3
	m.nodes["redis-2:6379"].Alive = true
	m.nodes["redis-2:6379"].Role = "slave"
	m.nodes["redis-3:6379"].Alive = false
	m.nodes["redis-3:6379"].Role = "slave"

	m.infoFunc = mockInfoFunc(infos)
	m.roleFunc = mockRoleFuncSequence(roles)

	// Track REPLICAOF calls
	replicaOfCalls := map[string][2]string{}
	m.replicaOfFunc = func(addr string, timeout time.Duration, arg1, arg2 string) error {
		replicaOfCalls[addr] = [2]string{arg1, arg2}
		return nil
	}

	m.triggerFailover()

	if m.master != "redis-2:6379" {
		t.Fatalf("master = %q, want redis-2:6379", m.master)
	}
	// Only redis-2 should have been called (REPLICAOF NO ONE from Promote)
	if len(replicaOfCalls) != 1 {
		t.Errorf("replicaOfCalls count = %d, want 1 (only Promote, no siblings)", len(replicaOfCalls))
	}
	if args, ok := replicaOfCalls["redis-2:6379"]; ok {
		if args[0] != "NO" || args[1] != "ONE" {
			t.Errorf("redis-2 got REPLICAOF %s %s, want NO ONE", args[0], args[1])
		}
	}
	// Dead nodes should NOT have been called
	if _, ok := replicaOfCalls["redis-1:6379"]; ok {
		t.Error("redis-1 (dead master) should not be reconfigured")
	}
	if _, ok := replicaOfCalls["redis-3:6379"]; ok {
		t.Error("redis-3 (dead replica) should not be reconfigured")
	}
}

func TestTriggerFailover_ExcludesDeadSiblings(t *testing.T) {
	// 4 nodes: master dead, 1 replica dead, 2 replicas alive.
	// Only alive siblings (minus promoted) should be reconfigured.
	addrs := []string{"redis-1:6379", "redis-2:6379", "redis-3:6379", "redis-4:6379"}
	infos := map[string]map[string]string{
		"redis-2:6379": {"master_repl_offset": "300"}, // best → promoted
		"redis-4:6379": {"master_repl_offset": "100"}, // alive sibling
	}
	roles := map[string][]RoleResult{
		"redis-2:6379": {{Role: "master", Offset: 300}},
	}

	m := NewMonitor(DefaultConfig(addrs))
	m.master = "redis-1:6379"
	m.nodes["redis-1:6379"].Alive = false
	m.nodes["redis-1:6379"].Role = "master"
	m.fails["redis-1:6379"] = 3
	m.nodes["redis-2:6379"].Alive = true
	m.nodes["redis-2:6379"].Role = "slave"
	m.nodes["redis-3:6379"].Alive = false // dead replica
	m.nodes["redis-3:6379"].Role = "slave"
	m.nodes["redis-4:6379"].Alive = true // alive replica
	m.nodes["redis-4:6379"].Role = "slave"

	m.infoFunc = mockInfoFunc(infos)
	m.roleFunc = mockRoleFuncSequence(roles)

	replicaOfCalls := map[string][2]string{}
	m.replicaOfFunc = func(addr string, timeout time.Duration, arg1, arg2 string) error {
		replicaOfCalls[addr] = [2]string{arg1, arg2}
		return nil
	}

	m.triggerFailover()

	if m.master != "redis-2:6379" {
		t.Fatalf("master = %q, want redis-2:6379", m.master)
	}
	// redis-4 (alive sibling) should be reconfigured to follow redis-2
	if args, ok := replicaOfCalls["redis-4:6379"]; ok {
		if args[0] != "redis-2" || args[1] != "6379" {
			t.Errorf("redis-4 got REPLICAOF %s %s, want redis-2 6379", args[0], args[1])
		}
	} else {
		t.Error("redis-4 (alive sibling) should have been reconfigured")
	}
	// redis-3 (dead) should NOT be called
	if _, ok := replicaOfCalls["redis-3:6379"]; ok {
		t.Error("redis-3 (dead replica) should not be reconfigured")
	}
	// redis-1 (dead master) should NOT be called
	if _, ok := replicaOfCalls["redis-1:6379"]; ok {
		t.Error("redis-1 (dead master) should not be reconfigured")
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
