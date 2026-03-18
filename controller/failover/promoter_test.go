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
