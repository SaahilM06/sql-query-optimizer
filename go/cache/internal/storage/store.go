package storage

import (
	"context"
	"hash/fnv"
	"time"

	"sqlopt/cache/internal/metrics"
)

// ShardedStore is the cache's key/value store: keys are distributed across
// a fixed number of shards by hash, so unrelated keys can be read/written
// concurrently without contending on the same lock, and each shard
// maintains its own LRU list independently.
type ShardedStore struct {
	shards  []*shard
	metrics *metrics.Metrics
}

// New creates a store with numShards shards, splitting maxMemoryBytes
// evenly across them as each shard's individual eviction budget.
func New(numShards int, maxMemoryBytes int64, m *metrics.Metrics) *ShardedStore {
	if numShards < 1 {
		numShards = 1
	}
	perShardBytes := maxMemoryBytes / int64(numShards)
	if perShardBytes < 1 {
		perShardBytes = 1
	}

	shards := make([]*shard, numShards)
	for i := range shards {
		shards[i] = newShard(perShardBytes, m)
	}
	return &ShardedStore{shards: shards, metrics: m}
}

func (s *ShardedStore) shardFor(key string) *shard {
	h := fnv.New32a()
	_, _ = h.Write([]byte(key))
	return s.shards[h.Sum32()%uint32(len(s.shards))]
}

func (s *ShardedStore) Get(key string) ([]byte, bool) {
	return s.shardFor(key).get(key)
}

func (s *ShardedStore) Exists(key string) bool {
	return s.shardFor(key).exists(key)
}

// Set stores key -> value under opts, reporting whether the write was
// actually applied (false only when an NX/XX condition blocks it).
func (s *ShardedStore) Set(key string, value []byte, opts SetOptions) bool {
	return s.shardFor(key).set(key, value, opts)
}

// Delete removes each of keys, returning how many were actually present.
func (s *ShardedStore) Delete(keys ...string) int {
	var count int
	for _, key := range keys {
		if s.shardFor(key).delete(key) {
			count++
		}
	}
	return count
}

func (s *ShardedStore) Expire(key string, ttl time.Duration) bool {
	return s.shardFor(key).expire(key, ttl)
}

func (s *ShardedStore) Persist(key string) bool {
	return s.shardFor(key).persist(key)
}

func (s *ShardedStore) TTL(key string) (time.Duration, bool) {
	return s.shardFor(key).ttl(key)
}

func (s *ShardedStore) IncrBy(key string, delta int64) (int64, error) {
	return s.shardFor(key).incrBy(key, delta)
}

// RestoreEntry inserts a pre-built entry directly (used by snapshot
// loading, which already has an absolute ExpiresAt to preserve).
func (s *ShardedStore) RestoreEntry(key string, ent *CacheEntry) {
	s.shardFor(key).restoreEntry(key, ent)
}

// ForEach visits every live entry across all shards (used by snapshot
// saving). fn must not call back into the store.
func (s *ShardedStore) ForEach(fn func(key string, ent *CacheEntry)) {
	for _, sh := range s.shards {
		sh.forEach(fn)
	}
}

func (s *ShardedStore) DBSize() int {
	var total int
	for _, sh := range s.shards {
		total += sh.dbSize()
	}
	return total
}

func (s *ShardedStore) FlushAll() {
	for _, sh := range s.shards {
		sh.flushAll()
	}
}

// RunExpirationWorker actively sweeps every shard for expired keys on
// interval, until ctx is canceled. This is a full per-shard scan each
// tick, which is simple and correct; the trade-off documented for anyone
// extending this is that it costs O(entries) per shard per tick rather
// than the sampling approach real Redis uses to bound worst-case pause
// time on very large keyspaces -- not a concern at this cache's scale.
func (s *ShardedStore) RunExpirationWorker(ctx context.Context, interval time.Duration) {
	ticker := time.NewTicker(interval)
	defer ticker.Stop()

	for {
		select {
		case <-ticker.C:
			for _, sh := range s.shards {
				sh.sweepExpired()
			}
		case <-ctx.Done():
			return
		}
	}
}
