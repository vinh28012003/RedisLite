//go:build unit

package failover

import (
	"testing"
	"time"
)

// --- encodeRESP ---

func TestEncodeRESP_SingleArg(t *testing.T) {
	got := string(encodeRESP("PING"))
	want := "*1\r\n$4\r\nPING\r\n"
	if got != want {
		t.Errorf("encodeRESP(PING) = %q, want %q", got, want)
	}
}

func TestEncodeRESP_MultipleArgs(t *testing.T) {
	got := string(encodeRESP("SET", "foo", "bar"))
	want := "*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n"
	if got != want {
		t.Errorf("encodeRESP(SET,foo,bar) = %q, want %q", got, want)
	}
}

func TestEncodeRESP_EmptyArg(t *testing.T) {
	got := string(encodeRESP("SET", "", "val"))
	want := "*3\r\n$3\r\nSET\r\n$0\r\n\r\n$3\r\nval\r\n"
	if got != want {
		t.Errorf("encodeRESP with empty arg = %q, want %q", got, want)
	}
}

func TestEncodeRESP_NoArgs(t *testing.T) {
	got := string(encodeRESP())
	want := "*0\r\n"
	if got != want {
		t.Errorf("encodeRESP() = %q, want %q", got, want)
	}
}

func TestEncodeRESP_LongArg(t *testing.T) {
	long := "abcdefghijklmnopqrstuvwxyz0123456789"
	got := string(encodeRESP("SET", "key", long))
	// Verify length encoding is correct
	if len(got) == 0 {
		t.Fatal("empty output")
	}
	// Should contain $36 for the long arg
	want := "$36\r\n"
	if !contains(got, want) {
		t.Errorf("expected %q in output for 36-byte arg", want)
	}
}

func TestEncodeRESP_ReplicaOf(t *testing.T) {
	got := string(encodeRESP("REPLICAOF", "NO", "ONE"))
	want := "*3\r\n$9\r\nREPLICAOF\r\n$2\r\nNO\r\n$3\r\nONE\r\n"
	if got != want {
		t.Errorf("encodeRESP(REPLICAOF,NO,ONE) = %q, want %q", got, want)
	}
}

// --- Ping ---

func TestPing_UnreachableReturnsFalse(t *testing.T) {
	result := Ping("127.0.0.1:1", 100*time.Millisecond)
	if result {
		t.Error("Ping to unreachable addr should return false")
	}
}

// --- Role ---

func TestRole_UnreachableReturnsError(t *testing.T) {
	_, err := Role("127.0.0.1:1", 100*time.Millisecond)
	if err == nil {
		t.Error("Role to unreachable addr should return error")
	}
}

// --- Info ---

func TestInfo_UnreachableReturnsError(t *testing.T) {
	_, err := Info("127.0.0.1:1", 100*time.Millisecond)
	if err == nil {
		t.Error("Info to unreachable addr should return error")
	}
}

// --- ReplicaOf ---

func TestReplicaOf_UnreachableReturnsError(t *testing.T) {
	err := ReplicaOf("127.0.0.1:1", 100*time.Millisecond, "NO", "ONE")
	if err == nil {
		t.Error("ReplicaOf to unreachable addr should return error")
	}
}

// helper
func contains(s, substr string) bool {
	return len(s) >= len(substr) && searchString(s, substr)
}

func searchString(s, sub string) bool {
	for i := 0; i <= len(s)-len(sub); i++ {
		if s[i:i+len(sub)] == sub {
			return true
		}
	}
	return false
}
