// Package storage implements the cache's core key/value store: a
// memory-bounded, sharded map with per-key TTL and LRU eviction.
package storage

import "time"

// CacheEntry is one stored key's value plus its expiration and size
// bookkeeping. Values are opaque bytes -- this cache stores serialized
// blobs (eventually query plans), not Redis's richer list/set/hash types.
type CacheEntry struct {
	Value     []byte
	ExpiresAt time.Time
	HasExpiry bool
	SizeBytes int64
}

// approxSize estimates the memory footprint of one entry: key + value +a
// fixed overhead for the entry struct and its map/list bookkeeping. This
// doesn't have to be exact, only consistent, so eviction behaves
// predictably under a configured memory budget.
const entryOverheadBytes = 64

func approxSize(key string, value []byte) int64 {
	return int64(len(key)) + int64(len(value)) + entryOverheadBytes
}
