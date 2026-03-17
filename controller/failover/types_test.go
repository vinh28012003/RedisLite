//go:build unit

package failover

import (
	"testing"
	"time"
)

func TestDefaultConfig_Values(t *testing.T) {
	nodes := []string{"redis-1:6379", "redis-2:6379", "redis-3:6379"}
	cfg := DefaultConfig(nodes)

	if len(cfg.Nodes) != 3 {
		t.Errorf("expected 3 nodes, got %d", len(cfg.Nodes))
	}
	if cfg.PingInterval != 500*time.Millisecond {
		t.Errorf("expected PingInterval 500ms, got %v", cfg.PingInterval)
	}
	if cfg.PingTimeout != 500*time.Millisecond {
		t.Errorf("expected PingTimeout 500ms, got %v", cfg.PingTimeout)
	}
	if cfg.FailureThresh != 3 {
		t.Errorf("expected FailureThresh 3, got %d", cfg.FailureThresh)
	}
}

func TestDefaultConfig_FailoverUnder5Seconds(t *testing.T) {
	cfg := DefaultConfig([]string{"a:1"})
	// Detection time = FailureThresh * PingInterval
	detectionTime := time.Duration(cfg.FailureThresh) * cfg.PingInterval
	if detectionTime > 2*time.Second {
		t.Errorf("detection time %v exceeds 2s budget (part of <5s failover)", detectionTime)
	}
}

func TestDefaultConfig_PreservesNodeOrder(t *testing.T) {
	nodes := []string{"z:1", "a:2", "m:3"}
	cfg := DefaultConfig(nodes)
	for i, n := range nodes {
		if cfg.Nodes[i] != n {
			t.Errorf("node[%d] = %s, want %s", i, cfg.Nodes[i], n)
		}
	}
}

func TestNode_ZeroValues(t *testing.T) {
	n := Node{Addr: "redis-1:6379"}
	if n.Role != "" {
		t.Errorf("expected empty Role, got %q", n.Role)
	}
	if n.Alive {
		t.Error("expected Alive=false by default")
	}
}
