package failover

import (
	"fmt"
	"log"
	"sort"
	"strconv"
	"time"
)

// InfoFunc queries INFO replication on a node. Seam for testing.
type InfoFunc func(addr string, timeout time.Duration) (map[string]string, error)

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
