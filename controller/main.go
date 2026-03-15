package main

import (
	"log"
	"os"
	"strings"

	"github.com/vinh28012003/RedisLite/controller/failover"
)

func main() {
	// Parse REDIS_NODES from environment: "redis-1:6379,redis-2:6379,redis-3:6379"
	nodesEnv := os.Getenv("REDIS_NODES")
	if nodesEnv == "" {
		log.Fatal("REDIS_NODES environment variable required (comma-separated host:port list)")
	}

	nodes := strings.Split(nodesEnv, ",")
	for i := range nodes {
		nodes[i] = strings.TrimSpace(nodes[i])
	}

	log.Printf("RedisLite Failover Controller starting with nodes: %v", nodes)

	cfg := failover.DefaultConfig(nodes)
	monitor := failover.NewMonitor(cfg)
	monitor.Run()
}
