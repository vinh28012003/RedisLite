//go:build unit

package failover

import (
	"fmt"
	"testing"
	"time"
)

// mockInfoFunc builds an InfoFunc from a map of addr → info key-values.
// Addrs not in the map return an error (simulates unreachable node).
func mockInfoFunc(infos map[string]map[string]string) InfoFunc {
	return func(addr string, timeout time.Duration) (map[string]string, error) {
		if info, ok := infos[addr]; ok {
			return info, nil
		}
		return nil, fmt.Errorf("connect %s: connection refused", addr)
	}
}

func TestPickBestReplica_HappyPath(t *testing.T) {
	// 3 replicas with different offsets → highest first
	infos := map[string]map[string]string{
		"redis-2:6379": {"master_repl_offset": "100"},
		"redis-3:6379": {"master_repl_offset": "300"},
		"redis-4:6379": {"master_repl_offset": "200"},
	}
	candidates := []string{"redis-2:6379", "redis-3:6379", "redis-4:6379"}

	ranked, err := PickBestReplica(candidates, 500*time.Millisecond, mockInfoFunc(infos))
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}

	if len(ranked) != 3 {
		t.Fatalf("ranked len = %d, want 3", len(ranked))
	}
	if ranked[0] != "redis-3:6379" {
		t.Errorf("best = %q, want redis-3:6379 (offset 300)", ranked[0])
	}
	if ranked[1] != "redis-4:6379" {
		t.Errorf("second = %q, want redis-4:6379 (offset 200)", ranked[1])
	}
	if ranked[2] != "redis-2:6379" {
		t.Errorf("third = %q, want redis-2:6379 (offset 100)", ranked[2])
	}
}

func TestPickBestReplica_Tiebreak(t *testing.T) {
	// 2 replicas with same offset → sorted by addr ASC
	infos := map[string]map[string]string{
		"redis-3:6379": {"master_repl_offset": "200"},
		"redis-2:6379": {"master_repl_offset": "200"},
	}
	candidates := []string{"redis-3:6379", "redis-2:6379"}

	ranked, err := PickBestReplica(candidates, 500*time.Millisecond, mockInfoFunc(infos))
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}

	if ranked[0] != "redis-2:6379" {
		t.Errorf("best = %q, want redis-2:6379 (tiebreak by addr)", ranked[0])
	}
	if ranked[1] != "redis-3:6379" {
		t.Errorf("second = %q, want redis-3:6379", ranked[1])
	}
}

func TestPickBestReplica_OneUnreachable(t *testing.T) {
	// 3 candidates, one unreachable → skipped, remaining ranked
	infos := map[string]map[string]string{
		"redis-2:6379": {"master_repl_offset": "100"},
		// redis-3 missing → unreachable
		"redis-4:6379": {"master_repl_offset": "300"},
	}
	candidates := []string{"redis-2:6379", "redis-3:6379", "redis-4:6379"}

	ranked, err := PickBestReplica(candidates, 500*time.Millisecond, mockInfoFunc(infos))
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}

	if len(ranked) != 2 {
		t.Fatalf("ranked len = %d, want 2 (one skipped)", len(ranked))
	}
	if ranked[0] != "redis-4:6379" {
		t.Errorf("best = %q, want redis-4:6379", ranked[0])
	}
}

func TestPickBestReplica_AllUnreachable(t *testing.T) {
	// All candidates unreachable → error
	infos := map[string]map[string]string{} // empty → all fail
	candidates := []string{"redis-2:6379", "redis-3:6379"}

	ranked, err := PickBestReplica(candidates, 500*time.Millisecond, mockInfoFunc(infos))
	if err == nil {
		t.Fatal("expected error, got nil")
	}
	if len(ranked) != 0 {
		t.Errorf("ranked len = %d, want 0", len(ranked))
	}
}

func TestPickBestReplica_EmptyCandidates(t *testing.T) {
	// No candidates at all → error
	ranked, err := PickBestReplica([]string{}, 500*time.Millisecond, mockInfoFunc(nil))
	if err == nil {
		t.Fatal("expected error, got nil")
	}
	if len(ranked) != 0 {
		t.Errorf("ranked len = %d, want 0", len(ranked))
	}
}

func TestPickBestReplica_BadOffset(t *testing.T) {
	// One replica has unparseable offset → treated as 0, still included
	infos := map[string]map[string]string{
		"redis-2:6379": {"master_repl_offset": "not-a-number"},
		"redis-3:6379": {"master_repl_offset": "200"},
	}
	candidates := []string{"redis-2:6379", "redis-3:6379"}

	ranked, err := PickBestReplica(candidates, 500*time.Millisecond, mockInfoFunc(infos))
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if len(ranked) != 2 {
		t.Fatalf("ranked len = %d, want 2", len(ranked))
	}
	if ranked[0] != "redis-3:6379" {
		t.Errorf("best = %q, want redis-3:6379 (offset 200 beats 0)", ranked[0])
	}
	if ranked[1] != "redis-2:6379" {
		t.Errorf("second = %q, want redis-2:6379 (bad offset treated as 0)", ranked[1])
	}
}

// --- Promote Tests ---

// mockReplicaOfFunc builds a ReplicaOfFunc. Addrs in the success set return nil.
// Others return an error (simulates unreachable node or command failure).
func mockReplicaOfFunc(success map[string]bool) ReplicaOfFunc {
	return func(addr string, timeout time.Duration, arg1, arg2 string) error {
		if success[addr] {
			return nil
		}
		return fmt.Errorf("REPLICAOF %s %s on %s: connection refused", arg1, arg2, addr)
	}
}

// mockRoleFuncSequence returns different RoleResults per call for a given addr.
// Each call advances the sequence. After exhausting the sequence, returns the last entry.
func mockRoleFuncSequence(seq map[string][]RoleResult) func(string, time.Duration) (RoleResult, error) {
	calls := make(map[string]int)
	return func(addr string, timeout time.Duration) (RoleResult, error) {
		results, ok := seq[addr]
		if !ok {
			return RoleResult{}, fmt.Errorf("connect %s: connection refused", addr)
		}
		idx := calls[addr]
		if idx >= len(results) {
			idx = len(results) - 1 // stay on last entry
		}
		calls[addr]++
		return results[idx], nil
	}
}

func TestPromote_HappyPath_FirstAttempt(t *testing.T) {
	// First candidate promotes successfully on first ROLE check
	ranked := []string{"redis-2:6379"}
	replicaOf := mockReplicaOfFunc(map[string]bool{"redis-2:6379": true})
	roleFunc := mockRoleFuncSequence(map[string][]RoleResult{
		"redis-2:6379": {{Role: "master", Offset: 100}},
	})

	addr, err := Promote(ranked, 500*time.Millisecond, replicaOf, roleFunc)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if addr != "redis-2:6379" {
		t.Errorf("promoted = %q, want redis-2:6379", addr)
	}
}

func TestPromote_VerificationOnRetry(t *testing.T) {
	// ROLE returns "slave" first, then "master" on second poll
	ranked := []string{"redis-2:6379"}
	replicaOf := mockReplicaOfFunc(map[string]bool{"redis-2:6379": true})
	roleFunc := mockRoleFuncSequence(map[string][]RoleResult{
		"redis-2:6379": {
			{Role: "slave", Offset: 100},  // attempt 1: still replicating
			{Role: "master", Offset: 100}, // attempt 2: promoted
		},
	})

	addr, err := Promote(ranked, 500*time.Millisecond, replicaOf, roleFunc)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if addr != "redis-2:6379" {
		t.Errorf("promoted = %q, want redis-2:6379", addr)
	}
}

func TestPromote_FallbackOnReplicaOfFailure(t *testing.T) {
	// First candidate's REPLICAOF fails → falls back to second candidate
	ranked := []string{"redis-2:6379", "redis-3:6379"}
	replicaOf := mockReplicaOfFunc(map[string]bool{
		"redis-2:6379": false, // REPLICAOF fails
		"redis-3:6379": true,  // REPLICAOF succeeds
	})
	roleFunc := mockRoleFuncSequence(map[string][]RoleResult{
		"redis-3:6379": {{Role: "master", Offset: 80}},
	})

	addr, err := Promote(ranked, 500*time.Millisecond, replicaOf, roleFunc)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if addr != "redis-3:6379" {
		t.Errorf("promoted = %q, want redis-3:6379 (fallback)", addr)
	}
}

func TestPromote_FallbackOnVerificationFailure(t *testing.T) {
	// First candidate: REPLICAOF succeeds but never becomes master (3 polls all "slave")
	// Second candidate: succeeds normally
	ranked := []string{"redis-2:6379", "redis-3:6379"}
	replicaOf := mockReplicaOfFunc(map[string]bool{
		"redis-2:6379": true,
		"redis-3:6379": true,
	})
	roleFunc := mockRoleFuncSequence(map[string][]RoleResult{
		"redis-2:6379": {
			{Role: "slave", Offset: 100}, // poll 1
			{Role: "slave", Offset: 100}, // poll 2
			{Role: "slave", Offset: 100}, // poll 3 — exhausted, give up
		},
		"redis-3:6379": {{Role: "master", Offset: 80}},
	})

	addr, err := Promote(ranked, 500*time.Millisecond, replicaOf, roleFunc)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if addr != "redis-3:6379" {
		t.Errorf("promoted = %q, want redis-3:6379 (fallback after verification failure)", addr)
	}
}

func TestPromote_AllCandidatesFail(t *testing.T) {
	// All candidates fail — REPLICAOF fails for all
	ranked := []string{"redis-2:6379", "redis-3:6379"}
	replicaOf := mockReplicaOfFunc(map[string]bool{}) // all fail
	roleFunc := mockRoleFuncSequence(map[string][]RoleResult{})

	addr, err := Promote(ranked, 500*time.Millisecond, replicaOf, roleFunc)
	if err == nil {
		t.Fatal("expected error, got nil")
	}
	if addr != "" {
		t.Errorf("addr = %q, want empty on failure", addr)
	}
}

func TestPromote_EmptyRankedList(t *testing.T) {
	// No candidates at all
	addr, err := Promote([]string{}, 500*time.Millisecond, nil, nil)
	if err == nil {
		t.Fatal("expected error, got nil")
	}
	if addr != "" {
		t.Errorf("addr = %q, want empty", addr)
	}
}

func TestPromote_RoleQueryErrors_StillRetries(t *testing.T) {
	// ROLE query errors on attempts 1-2, succeeds on attempt 3
	ranked := []string{"redis-2:6379"}
	replicaOf := mockReplicaOfFunc(map[string]bool{"redis-2:6379": true})

	callCount := 0
	roleFunc := func(addr string, timeout time.Duration) (RoleResult, error) {
		callCount++
		if callCount <= 2 {
			return RoleResult{}, fmt.Errorf("timeout reading from %s", addr)
		}
		return RoleResult{Role: "master", Offset: 100}, nil
	}

	addr, err := Promote(ranked, 500*time.Millisecond, replicaOf, roleFunc)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if addr != "redis-2:6379" {
		t.Errorf("promoted = %q, want redis-2:6379", addr)
	}
	if callCount != 3 {
		t.Errorf("ROLE called %d times, want 3", callCount)
	}
}

// --- Reconfigure Tests ---

func TestReconfigure_HappyPath(t *testing.T) {
	// 3 siblings all reachable → all reconfigured, no errors
	called := map[string][2]string{}
	replicaOf := func(addr string, timeout time.Duration, arg1, arg2 string) error {
		called[addr] = [2]string{arg1, arg2}
		return nil
	}

	siblings := []string{"redis-3:6379", "redis-4:6379", "redis-5:6379"}
	errs := Reconfigure(siblings, "redis-2:6379", 500*time.Millisecond, replicaOf)

	for i, err := range errs {
		if err != nil {
			t.Errorf("errs[%d] = %v, want nil", i, err)
		}
	}
	// Verify correct host/port sent
	for _, addr := range siblings {
		args, ok := called[addr]
		if !ok {
			t.Errorf("%s was not called", addr)
			continue
		}
		if args[0] != "redis-2" || args[1] != "6379" {
			t.Errorf("%s got REPLICAOF %s %s, want redis-2 6379", addr, args[0], args[1])
		}
	}
}

func TestReconfigure_OneSiblingUnreachable(t *testing.T) {
	// 3 siblings, one fails — best-effort: others still reconfigured
	replicaOf := func(addr string, timeout time.Duration, arg1, arg2 string) error {
		if addr == "redis-4:6379" {
			return fmt.Errorf("connection refused")
		}
		return nil
	}

	siblings := []string{"redis-3:6379", "redis-4:6379", "redis-5:6379"}
	errs := Reconfigure(siblings, "redis-2:6379", 500*time.Millisecond, replicaOf)

	if errs[0] != nil {
		t.Errorf("errs[0] = %v, want nil (redis-3 should succeed)", errs[0])
	}
	if errs[1] == nil {
		t.Error("errs[1] = nil, want error (redis-4 unreachable)")
	}
	if errs[2] != nil {
		t.Errorf("errs[2] = %v, want nil (redis-5 should succeed)", errs[2])
	}
}

func TestReconfigure_AllSiblingsUnreachable(t *testing.T) {
	// All siblings fail — errors returned for all, no panic
	replicaOf := func(addr string, timeout time.Duration, arg1, arg2 string) error {
		return fmt.Errorf("connection refused")
	}

	siblings := []string{"redis-3:6379", "redis-4:6379"}
	errs := Reconfigure(siblings, "redis-2:6379", 500*time.Millisecond, replicaOf)

	for i, err := range errs {
		if err == nil {
			t.Errorf("errs[%d] = nil, want error", i)
		}
	}
}

func TestReconfigure_EmptySiblings(t *testing.T) {
	// No siblings to reconfigure — returns empty slice
	errs := Reconfigure([]string{}, "redis-2:6379", 500*time.Millisecond, nil)

	if len(errs) != 0 {
		t.Errorf("len(errs) = %d, want 0", len(errs))
	}
}

func TestReconfigure_BadMasterAddr(t *testing.T) {
	// Malformed master address — all siblings get error
	siblings := []string{"redis-3:6379", "redis-4:6379"}
	errs := Reconfigure(siblings, "no-port-here", 500*time.Millisecond, nil)

	for i, err := range errs {
		if err == nil {
			t.Errorf("errs[%d] = nil, want error (bad master addr)", i)
		}
	}
}
