package failover

import (
	"fmt"
	"net"
	"strings"
	"time"
)

// encodeRESP encodes a command as a RESP array: *N\r\n$len\r\narg\r\n...
func encodeRESP(args ...string) []byte {
	var b strings.Builder
	fmt.Fprintf(&b, "*%d\r\n", len(args))
	for _, arg := range args {
		fmt.Fprintf(&b, "$%d\r\n%s\r\n", len(arg), arg)
	}
	return []byte(b.String())
}

// sendCommand connects to addr, sends a RESP command, returns raw response.
// Each call opens a new TCP connection (simple, no pooling needed for controller).
func sendCommand(addr string, timeout time.Duration, args ...string) (string, error) {
	conn, err := net.DialTimeout("tcp", addr, timeout)
	if err != nil {
		return "", fmt.Errorf("connect %s: %w", addr, err)
	}
	defer conn.Close()
	conn.SetDeadline(time.Now().Add(timeout))

	_, err = conn.Write(encodeRESP(args...))
	if err != nil {
		return "", fmt.Errorf("write %s: %w", addr, err)
	}

	buf := make([]byte, 4096)
	n, err := conn.Read(buf)
	if err != nil {
		return "", fmt.Errorf("read %s: %w", addr, err)
	}
	return string(buf[:n]), nil
}

// Ping sends PING and returns true if +PONG received.
func Ping(addr string, timeout time.Duration) bool {
	resp, err := sendCommand(addr, timeout, "PING")
	if err != nil {
		return false
	}
	return strings.Contains(resp, "+PONG")
}

// TODO: You'll implement these — they shape the controller's decision-making.
//
// Role sends ROLE command, returns ("master", offset) or ("slave", host, port, state, offset).
// Use this to determine current topology during health checks.
//
// Info sends INFO replication, parses key fields.
// Use this for replica offset comparison when choosing promotion target.
//
// ReplicaOf sends REPLICAOF command for promotion (NO ONE) or reconfiguration (host port).
// This is the core action the controller takes during failover.
