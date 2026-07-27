package storage

import (
	"fmt"
	"strconv"
	"sync"
	"time"

	"sqlopt/cache/internal/metrics"
)

// shard is one partition of the sharded store: its own lock, its own
// key/value map, its own LRU list, and its own slice of the global memory
// budget. Every operation here holds shard.mu -- callers never touch
// entries/lru directly.
type shard struct {
	mu        sync.Mutex
	entries   map[string]*CacheEntry
	lru       *lru
	usedBytes int64
	maxBytes  int64
	metrics   *metrics.Metrics
}

func newShard(maxBytes int64, m *metrics.Metrics) *shard {
	return &shard{
		entries:  make(map[string]*CacheEntry),
		lru:      newLRU(),
		maxBytes: maxBytes,
		metrics:  m,
	}
}

// isExpired reports whether ent has passed its TTL. Caller holds s.mu.
func isExpired(ent *CacheEntry, now time.Time) bool {
	return ent.HasExpiry && now.After(ent.ExpiresAt)
}

// removeLocked deletes key from both the map and the LRU list, updating
// byte accounting. Caller holds s.mu.
func (s *shard) removeLocked(key string) {
	ent, ok := s.entries[key]
	if !ok {
		return
	}
	delete(s.entries, key)
	s.lru.remove(key)
	s.usedBytes -= ent.SizeBytes
	s.metrics.CurrentEntries.Add(-1)
	s.metrics.MemoryBytes.Add(-ent.SizeBytes)
}

func (s *shard) get(key string) ([]byte, bool) {
	s.mu.Lock()
	defer s.mu.Unlock()

	ent, ok := s.entries[key]
	if !ok {
		s.metrics.CacheMisses.Add(1)
		return nil, false
	}
	if isExpired(ent, time.Now()) {
		s.removeLocked(key)
		s.metrics.ExpiredKeys.Add(1)
		s.metrics.CacheMisses.Add(1)
		return nil, false
	}
	s.lru.touch(key)
	s.metrics.CacheHits.Add(1)
	return ent.Value, true
}

func (s *shard) exists(key string) bool {
	s.mu.Lock()
	defer s.mu.Unlock()

	ent, ok := s.entries[key]
	if !ok {
		return false
	}
	if isExpired(ent, time.Now()) {
		s.removeLocked(key)
		s.metrics.ExpiredKeys.Add(1)
		return false
	}
	return true
}

// SetOptions controls SET's conditional/expiry behavior, mirroring Redis's
// EX/PX/NX/XX modifiers.
type SetOptions struct {
	TTL    time.Duration // <= 0 means no expiry
	HasTTL bool
	NX     bool // only set if the key does not already exist
	XX     bool // only set if the key already exists
}

// set stores key -> value. Returns applied=false (no error) when an NX/XX
// condition blocks the write, matching Redis's "SET returns nil" behavior
// rather than treating it as an error.
func (s *shard) set(key string, value []byte, opts SetOptions) bool {
	s.mu.Lock()
	defer s.mu.Unlock()

	existing, exists := s.entries[key]
	if exists && isExpired(existing, time.Now()) {
		s.removeLocked(key)
		exists = false
	}

	if opts.NX && exists {
		return false
	}
	if opts.XX && !exists {
		return false
	}

	newSize := approxSize(key, value)
	if exists {
		s.usedBytes += newSize - existing.SizeBytes
		s.metrics.MemoryBytes.Add(newSize - existing.SizeBytes)
	} else {
		s.usedBytes += newSize
		s.metrics.CurrentEntries.Add(1)
		s.metrics.MemoryBytes.Add(newSize)
	}

	ent := &CacheEntry{Value: value, SizeBytes: newSize}
	if opts.HasTTL && opts.TTL > 0 {
		ent.HasExpiry = true
		ent.ExpiresAt = time.Now().Add(opts.TTL)
	}
	s.entries[key] = ent
	s.lru.touch(key)

	s.evictIfOverBudget()
	return true
}

// evictIfOverBudget evicts least-recently-used entries until usedBytes is
// back under the shard's share of the memory budget. Caller holds s.mu.
func (s *shard) evictIfOverBudget() {
	for s.usedBytes > s.maxBytes {
		key, ok := s.lru.evictCandidate()
		if !ok {
			return
		}
		s.removeLocked(key)
		s.metrics.EvictedKeys.Add(1)
	}
}

func (s *shard) delete(key string) bool {
	s.mu.Lock()
	defer s.mu.Unlock()

	if _, ok := s.entries[key]; !ok {
		return false
	}
	s.removeLocked(key)
	return true
}

func (s *shard) expire(key string, ttl time.Duration) bool {
	s.mu.Lock()
	defer s.mu.Unlock()

	ent, ok := s.entries[key]
	if !ok || isExpired(ent, time.Now()) {
		return false
	}
	ent.HasExpiry = true
	ent.ExpiresAt = time.Now().Add(ttl)
	return true
}

// persist removes key's TTL. Returns true only if a TTL was actually
// cleared (matching Redis's PERSIST: false for a missing key or one with
// no TTL to begin with).
func (s *shard) persist(key string) bool {
	s.mu.Lock()
	defer s.mu.Unlock()

	ent, ok := s.entries[key]
	if !ok || isExpired(ent, time.Now()) || !ent.HasExpiry {
		return false
	}
	ent.HasExpiry = false
	return true
}

// ttl returns the remaining TTL for key. ok is false if the key is absent
// or expired; a zero duration with ok=true means the key exists with no
// expiration.
func (s *shard) ttl(key string) (time.Duration, bool) {
	s.mu.Lock()
	defer s.mu.Unlock()

	ent, ok := s.entries[key]
	if !ok {
		return 0, false
	}
	if isExpired(ent, time.Now()) {
		s.removeLocked(key)
		s.metrics.ExpiredKeys.Add(1)
		return 0, false
	}
	if !ent.HasExpiry {
		return 0, true
	}
	return time.Until(ent.ExpiresAt), true
}

// incrBy parses key's current value as a base-10 int64 (treating a missing
// key as 0), adds delta, stores the result back as a decimal string, and
// returns the new value.
func (s *shard) incrBy(key string, delta int64) (int64, error) {
	s.mu.Lock()
	defer s.mu.Unlock()

	var current int64
	ent, ok := s.entries[key]
	if ok && !isExpired(ent, time.Now()) {
		parsed, err := strconv.ParseInt(string(ent.Value), 10, 64)
		if err != nil {
			return 0, fmt.Errorf("value is not an integer or out of range")
		}
		current = parsed
	}

	next := current + delta
	value := []byte(strconv.FormatInt(next, 10))
	newSize := approxSize(key, value)

	if ok {
		s.usedBytes += newSize - ent.SizeBytes
		s.metrics.MemoryBytes.Add(newSize - ent.SizeBytes)
		ent.Value = value
		ent.SizeBytes = newSize
		// INCR/DECR do not touch an existing TTL.
	} else {
		s.usedBytes += newSize
		s.metrics.CurrentEntries.Add(1)
		s.metrics.MemoryBytes.Add(newSize)
		s.entries[key] = &CacheEntry{Value: value, SizeBytes: newSize}
	}
	s.lru.touch(key)
	s.evictIfOverBudget()

	return next, nil
}

// restoreEntry inserts ent as-is (used by snapshot loading, which already
// carries an absolute ExpiresAt). Entries that are already expired are
// dropped rather than restored.
func (s *shard) restoreEntry(key string, ent *CacheEntry) {
	s.mu.Lock()
	defer s.mu.Unlock()

	if isExpired(ent, time.Now()) {
		return
	}
	s.entries[key] = ent
	s.lru.touch(key)
	s.usedBytes += ent.SizeBytes
	s.metrics.CurrentEntries.Add(1)
	s.metrics.MemoryBytes.Add(ent.SizeBytes)
}

// forEach calls fn for every live (non-expired) entry. Used by snapshot
// saving; fn must not call back into the shard.
func (s *shard) forEach(fn func(key string, ent *CacheEntry)) {
	s.mu.Lock()
	defer s.mu.Unlock()

	now := time.Now()
	for key, ent := range s.entries {
		if isExpired(ent, now) {
			continue
		}
		fn(key, ent)
	}
}

func (s *shard) dbSize() int {
	s.mu.Lock()
	defer s.mu.Unlock()
	return len(s.entries)
}

func (s *shard) flushAll() {
	s.mu.Lock()
	defer s.mu.Unlock()

	s.metrics.CurrentEntries.Add(-int64(len(s.entries)))
	s.metrics.MemoryBytes.Add(-s.usedBytes)
	s.entries = make(map[string]*CacheEntry)
	s.lru = newLRU()
	s.usedBytes = 0
}

// sweepExpired removes every currently-expired entry. Called periodically
// by the active-expiration worker so TTLs are enforced even for keys that
// are never touched again.
func (s *shard) sweepExpired() {
	s.mu.Lock()
	defer s.mu.Unlock()

	now := time.Now()
	var expired []string
	for key, ent := range s.entries {
		if isExpired(ent, now) {
			expired = append(expired, key)
		}
	}
	for _, key := range expired {
		s.removeLocked(key)
		s.metrics.ExpiredKeys.Add(1)
	}
}
