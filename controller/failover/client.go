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

// RoleResult holds parsed ROLE response.
type RoleResult struct {
	Role   string // "master" or "slave"
	Offset int64
	// Replica-only fields (zero for master)
	MasterHost string
	MasterPort string
}

// Role sends ROLE command and parses the response.
// Master returns: role="master", offset=N
// Replica returns: role="slave", host, port, offset
func Role(addr string, timeout time.Duration) (RoleResult, error) {
	resp, err := sendCommand(addr, timeout, "ROLE")
	if err != nil {
		return RoleResult{}, err
	}

	// RESP array — extract bulk strings and integers line by line
	lines := strings.Split(resp, "\r\n")
	var values []string
	for _, line := range lines {
		if len(line) == 0 || line[0] == '*' || line[0] == '$' {
			continue // skip array/bulk headers
		}
		if line[0] == ':' {
			values = append(values, line[1:]) // strip : prefix from integers
		} else {
			values = append(values, line)
		}
	}

	if len(values) < 2 {
		return RoleResult{}, fmt.Errorf("unexpected ROLE response from %s: %q", addr, resp)
	}

	result := RoleResult{Role: values[0]}
	fmt.Sscanf(values[1], "%d", &result.Offset)

	if result.Role == "slave" && len(values) >= 5 {
		result.MasterHost = values[2]
		result.MasterPort = values[3]
		// values[4] = "connected" state, values[5] = offset (if present)
	}

	return result, nil
}

// Info sends INFO replication and returns key-value pairs.
// Keys: role, master_replid, master_repl_offset, master_replid2, second_repl_offset
func Info(addr string, timeout time.Duration) (map[string]string, error) {
	resp, err := sendCommand(addr, timeout, "INFO", "replication")
	if err != nil {
		return nil, err
	}

	result := make(map[string]string)

	// Bulk string response: $N\r\n<body>\r\n
	// Body contains key:value lines separated by \r\n
	lines := strings.Split(resp, "\r\n")
	for _, line := range lines {
		if idx := strings.IndexByte(line, ':'); idx > 0 {
			result[line[:idx]] = line[idx+1:]
		}
	}

	return result, nil
}

// ReplicaOf sends REPLICAOF command. Use ("NO", "ONE") for promotion
// or ("host", "port") for reconfiguration.
func ReplicaOf(addr string, timeout time.Duration, arg1, arg2 string) error {
	resp, err := sendCommand(addr, timeout, "REPLICAOF", arg1, arg2)
	if err != nil {
		return err
	}
	if !strings.Contains(resp, "+OK") {
		return fmt.Errorf("REPLICAOF %s %s on %s: %q", arg1, arg2, addr, resp)
	}
	return nil
}
