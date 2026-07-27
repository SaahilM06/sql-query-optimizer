// Package config holds server-wide configuration, populated from CLI flags
// by cmd/server/main.go.
package config

import "time"

type Config struct {
	Address string // TCP listen address, e.g. ":6380"

	NumShards      int   // number of storage shards
	MaxMemoryBytes int64 // approximate total memory budget across all shards

	SnapshotPath     string        // where persistence snapshots are read/written; "" disables persistence
	SnapshotInterval time.Duration // 0 disables periodic auto-save (SAVE and shutdown-save still work)

	ExpirationSweepInterval time.Duration // how often the active-expiration worker scans each shard
}

func Default() Config {
	return Config{
		Address:                 ":6380",
		NumShards:               64,
		MaxMemoryBytes:          128 * 1024 * 1024, // 128 MiB
		SnapshotPath:            "cache.snapshot",
		SnapshotInterval:        5 * time.Minute,
		ExpirationSweepInterval: 250 * time.Millisecond,
	}
}
