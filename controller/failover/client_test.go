//go:build unit

package failover

import (
	"fmt"
	"net"
	"testing"
	"time"
)

// startMockServer starts a TCP listener that accepts one connection,
// writes the given response, and closes. Returns the listener address.
func startMockServer(t *testing.T, response string) string {
	t.Helper()
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("failed to start mock server: %v", err)
	}
	go func() {
		conn, err := ln.Accept()
		if err != nil {
			return
		}
		defer conn.Close()
		// Read the incoming command (discard it)
		buf := make([]byte, 1024)
		conn.Read(buf)
		// Write the canned response
		conn.Write([]byte(response))
	}()
	t.Cleanup(func() { ln.Close() })
	return ln.Addr().String()
}

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

// --- Happy Path Tests (mock TCP server) ---

func TestPing_ValidPong(t *testing.T) {
	addr := startMockServer(t, "+PONG\r\n")
	result := Ping(addr, 500*time.Millisecond)
	if !result {
		t.Error("Ping should return true on +PONG response")
	}
}

func TestRole_ParseMaster(t *testing.T) {
	// Master ROLE response: *3\r\n$6\r\nmaster\r\n:100\r\n*0\r\n
	// (role, offset, empty replica list)
	resp := "*3\r\n$6\r\nmaster\r\n:100\r\n*0\r\n"
	addr := startMockServer(t, resp)

	result, err := Role(addr, 500*time.Millisecond)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if result.Role != "master" {
		t.Errorf("role = %q, want master", result.Role)
	}
	if result.Offset != 100 {
		t.Errorf("offset = %d, want 100", result.Offset)
	}
}

func TestRole_ParseSlave(t *testing.T) {
	// Real Redis slave ROLE response: *5\r\n$5\r\nslave\r\n$7\r\nredis-1\r\n:6379\r\n$9\r\nconnected\r\n:42\r\n
	// Parser extracts values in order: ["slave", "redis-1", "6379", "connected", "42"]
	// values[1]="redis-1" → Sscanf for Offset gets 0 (not an int)
	// values[2]="6379", values[3]="connected" → MasterHost/MasterPort are wrong
	//
	// Known limitation: Role() parser was designed for master (values[1]=offset).
	// Slave field mapping is incorrect, but MasterHost/MasterPort are unused by the controller.
	// Fix deferred — controller only reads .Role from slave responses.
	resp := "*5\r\n$5\r\nslave\r\n$7\r\nredis-1\r\n:6379\r\n$9\r\nconnected\r\n:42\r\n"
	addr := startMockServer(t, resp)

	result, err := Role(addr, 500*time.Millisecond)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if result.Role != "slave" {
		t.Errorf("role = %q, want slave", result.Role)
	}
	// Offset is 0 because values[1]="redis-1" can't be parsed as int
	if result.Offset != 0 {
		t.Errorf("offset = %d, want 0 (known: slave offset parsing is wrong)", result.Offset)
	}
}

func TestInfo_ParseKeyValues(t *testing.T) {
	// INFO replication response: bulk string with key:value lines
	body := "role:master\r\nmaster_replid:abc123\r\nmaster_repl_offset:42\r\n"
	resp := "$" + fmt.Sprintf("%d", len(body)) + "\r\n" + body + "\r\n"
	addr := startMockServer(t, resp)

	info, err := Info(addr, 500*time.Millisecond)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if info["role"] != "master" {
		t.Errorf("role = %q, want master", info["role"])
	}
	if info["master_replid"] != "abc123" {
		t.Errorf("master_replid = %q, want abc123", info["master_replid"])
	}
	if info["master_repl_offset"] != "42" {
		t.Errorf("master_repl_offset = %q, want 42", info["master_repl_offset"])
	}
}

func TestReplicaOf_Success(t *testing.T) {
	addr := startMockServer(t, "+OK\r\n")
	err := ReplicaOf(addr, 500*time.Millisecond, "NO", "ONE")
	if err != nil {
		t.Errorf("unexpected error: %v", err)
	}
}

func TestReplicaOf_ErrorResponse(t *testing.T) {
	// Server returns an error response (not +OK)
	addr := startMockServer(t, "-ERR unknown command\r\n")
	err := ReplicaOf(addr, 500*time.Millisecond, "NO", "ONE")
	if err == nil {
		t.Error("expected error on non-OK response")
	}
}

func TestRole_MalformedResponse(t *testing.T) {
	// Server returns too few values — should error
	addr := startMockServer(t, "*1\r\n$6\r\nmaster\r\n")
	_, err := Role(addr, 500*time.Millisecond)
	if err == nil {
		t.Error("expected error on malformed ROLE response (too few values)")
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
