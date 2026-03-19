package failover

import (
	"fmt"
	"log"
	"net"
	"sort"
	"strconv"
	"time"
)

// InfoFunc queries INFO replication on a node. Seam for testing.
type InfoFunc func(addr string, timeout time.Duration) (map[string]string, error)

// ReplicaOfFunc sends REPLICAOF to a node. Seam for testing.
type ReplicaOfFunc func(addr string, timeout time.Duration, arg1, arg2 string) error

// Promotion tuning constants.
const (
	promoteMaxRetries = 3                       // ROLE poll attempts after REPLICAOF NO ONE
	promoteRetryDelay = 200 * time.Millisecond  // delay between ROLE polls
)

// replicaCandidate pairs an address with its replication offset for sorting.
type replicaCandidate struct {
	addr   string
	offset int64
}

// PickBestReplica queries INFO on each candidate and returns them ranked
// by replication offset (highest first). Tiebreak: addr ASC for determinism.
// Unreachable candidates are skipped. Returns error if no candidates are reachable.
func PickBestReplica(candidates []string, timeout time.Duration, infoFunc InfoFunc) ([]string, error) {
	if len(candidates) == 0 {
		return nil, fmt.Errorf("no candidates provided")
	}

	var reachable []replicaCandidate

	for _, addr := range candidates {
		info, err := infoFunc(addr, timeout)
		if err != nil {
			log.Printf(" [selection] %s unreachable, skipping: %v", addr, err)
			continue
		}

		var offset int64
		if raw, ok := info["master_repl_offset"]; ok {
			parsed, err := strconv.ParseInt(raw, 10, 64)
			if err != nil {
				log.Printf(" [selection] %s bad offset %q, treating as 0", addr, raw)
			} else {
				offset = parsed
			}
		}

		reachable = append(reachable, replicaCandidate{addr: addr, offset: offset})
		log.Printf(" [selection] %s offset=%d", addr, offset)
	}

	if len(reachable) == 0 {
		return nil, fmt.Errorf("all %d candidates unreachable", len(candidates))
	}

	// Sort: highest offset first, then addr ASC for tiebreak
	sort.Slice(reachable, func(i, j int) bool {
		if reachable[i].offset != reachable[j].offset {
			return reachable[i].offset > reachable[j].offset
		}
		return reachable[i].addr < reachable[j].addr
	})

	ranked := make([]string, len(reachable))
	for i, c := range reachable {
		ranked[i] = c.addr
	}

	log.Printf(" [selection] ranked: %v (best: %s, offset=%d)", ranked, reachable[0].addr, reachable[0].offset)
	return ranked, nil
}

// Promote walks the ranked candidate list and attempts promotion:
//  1. REPLICAOF NO ONE
//  2. Poll ROLE up to promoteMaxRetries times (promoteRetryDelay apart)
//  3. If role == "master" → success, return addr
//  4. Otherwise → try next candidate
//
// Returns the address of the newly promoted master, or error if all fail.
func Promote(ranked []string, timeout time.Duration,
	replicaOfFunc ReplicaOfFunc, roleFunc func(string, time.Duration) (RoleResult, error),
) (string, error) {
	if len(ranked) == 0 {
		return "", fmt.Errorf("no candidates to promote")
	}

	for i, addr := range ranked {
		log.Printf(" [promote] trying candidate %d/%d: %s", i+1, len(ranked), addr)

		// Step 1: tell the replica to stop replicating
		if err := replicaOfFunc(addr, timeout, "NO", "ONE"); err != nil {
			log.Printf(" [promote] %s REPLICAOF NO ONE failed: %v", addr, err)
			continue
		}

		// Step 2: poll ROLE to verify it became master
		promoted := false
		for attempt := 1; attempt <= promoteMaxRetries; attempt++ {
			time.Sleep(promoteRetryDelay)

			roleResult, err := roleFunc(addr, timeout)
			if err != nil {
				log.Printf(" [promote] %s ROLE check %d/%d failed: %v", addr, attempt, promoteMaxRetries, err)
				continue
			}
			if roleResult.Role == "master" {
				log.Printf(" [promote] %s confirmed master on attempt %d", addr, attempt)
				promoted = true
				break
			}
			log.Printf(" [promote] %s still role=%s (attempt %d/%d)", addr, roleResult.Role, attempt, promoteMaxRetries)
		}

		if promoted {
			return addr, nil
		}
		log.Printf(" [promote] %s failed verification, trying next candidate", addr)
	}

	return "", fmt.Errorf("all %d candidates failed promotion", len(ranked))
}

// Reconfigure sends REPLICAOF <host> <port> to each sibling so they follow
// the new master. Best-effort: failures are logged but don't abort.
// Returns a slice of errors (nil entry = success for that index).
func Reconfigure(siblings []string, newMasterAddr string, timeout time.Duration,
	replicaOfFunc ReplicaOfFunc) []error {

	host, port, err := net.SplitHostPort(newMasterAddr)
	if err != nil {
		errs := make([]error, len(siblings))
		for i := range siblings {
			errs[i] = fmt.Errorf("bad master addr %q: %v", newMasterAddr, err)
		}
		return errs
	}

	errs := make([]error, len(siblings))
	for i, addr := range siblings {
		if err := replicaOfFunc(addr, timeout, host, port); err != nil {
			log.Printf(" [reconfig] %s REPLICAOF %s %s failed: %v", addr, host, port, err)
			errs[i] = err
		} else {
			log.Printf(" [reconfig] %s → now replicating from %s:%s", addr, host, port)
		}
	}
	return errs
}
