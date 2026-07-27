package storage

import (
	"fmt"
	"sync"
	"testing"
	"time"

	"sqlopt/cache/internal/metrics"
)

func newTestStore(numShards int, maxBytes int64) *ShardedStore {
	return New(numShards, maxBytes, metrics.New())
}

func TestSetGetRoundTrip(t *testing.T) {
	s := newTestStore(4, 1<<20)
	s.Set("k", []byte("v"), SetOptions{})
	got, ok := s.Get("k")
	if !ok || string(got) != "v" {
		t.Fatalf("got %q ok=%v", got, ok)
	}
}

func TestGetMissingKey(t *testing.T) {
	s := newTestStore(4, 1<<20)
	if _, ok := s.Get("nope"); ok {
		t.Fatal("expected miss")
	}
}

func TestSetNXOnlySetsWhenAbsent(t *testing.T) {
	s := newTestStore(4, 1<<20)
	if !s.Set("k", []byte("first"), SetOptions{NX: true}) {
		t.Fatal("expected NX set to apply on a new key")
	}
	if s.Set("k", []byte("second"), SetOptions{NX: true}) {
		t.Fatal("expected NX set to be rejected on an existing key")
	}
	got, _ := s.Get("k")
	if string(got) != "first" {
		t.Fatalf("expected value unchanged by rejected NX set, got %q", got)
	}
}

func TestSetXXOnlySetsWhenPresent(t *testing.T) {
	s := newTestStore(4, 1<<20)
	if s.Set("k", []byte("v"), SetOptions{XX: true}) {
		t.Fatal("expected XX set to be rejected on a missing key")
	}
	s.Set("k", []byte("v1"), SetOptions{})
	if !s.Set("k", []byte("v2"), SetOptions{XX: true}) {
		t.Fatal("expected XX set to apply on an existing key")
	}
	got, _ := s.Get("k")
	if string(got) != "v2" {
		t.Fatalf("got %q", got)
	}
}

func TestExpireAndTTL(t *testing.T) {
	s := newTestStore(4, 1<<20)
	s.Set("k", []byte("v"), SetOptions{})

	ttl, ok := s.TTL("k")
	if !ok || ttl != 0 {
		t.Fatalf("expected no-expiry key to report ttl=0 ok=true, got ttl=%v ok=%v", ttl, ok)
	}

	if !s.Expire("k", 10*time.Second) {
		t.Fatal("expected Expire to succeed on existing key")
	}
	ttl, ok = s.TTL("k")
	if !ok || ttl <= 0 || ttl > 10*time.Second {
		t.Fatalf("expected ttl in (0,10s], got %v ok=%v", ttl, ok)
	}

	if s.Expire("missing", time.Second) {
		t.Fatal("expected Expire on missing key to fail")
	}
}

func TestPersistRemovesTTL(t *testing.T) {
	s := newTestStore(4, 1<<20)
	s.Set("k", []byte("v"), SetOptions{TTL: 10 * time.Second, HasTTL: true})

	if !s.Persist("k") {
		t.Fatal("expected Persist to clear an existing TTL")
	}
	ttl, ok := s.TTL("k")
	if !ok || ttl != 0 {
		t.Fatalf("expected no TTL after Persist, got %v ok=%v", ttl, ok)
	}
	if s.Persist("k") {
		t.Fatal("expected second Persist (no TTL left) to report false")
	}
}

func TestSetWithTTLExpires(t *testing.T) {
	s := newTestStore(4, 1<<20)
	s.Set("k", []byte("v"), SetOptions{TTL: 20 * time.Millisecond, HasTTL: true})

	if _, ok := s.Get("k"); !ok {
		t.Fatal("expected key present before expiry")
	}
	time.Sleep(40 * time.Millisecond)
	if _, ok := s.Get("k"); ok {
		t.Fatal("expected key expired")
	}
}

func TestDelete(t *testing.T) {
	s := newTestStore(4, 1<<20)
	s.Set("a", []byte("1"), SetOptions{})
	s.Set("b", []byte("2"), SetOptions{})

	n := s.Delete("a", "missing", "b")
	if n != 2 {
		t.Fatalf("expected 2 deleted, got %d", n)
	}
	if _, ok := s.Get("a"); ok {
		t.Fatal("expected 'a' gone")
	}
}

func TestIncrDecr(t *testing.T) {
	s := newTestStore(4, 1<<20)

	v, err := s.IncrBy("counter", 1)
	if err != nil || v != 1 {
		t.Fatalf("first incr: v=%d err=%v", v, err)
	}
	v, err = s.IncrBy("counter", 5)
	if err != nil || v != 6 {
		t.Fatalf("second incr: v=%d err=%v", v, err)
	}
	v, err = s.IncrBy("counter", -2)
	if err != nil || v != 4 {
		t.Fatalf("decr via negative delta: v=%d err=%v", v, err)
	}
}

func TestIncrOnNonIntegerFails(t *testing.T) {
	s := newTestStore(4, 1<<20)
	s.Set("k", []byte("not-a-number"), SetOptions{})
	if _, err := s.IncrBy("k", 1); err == nil {
		t.Fatal("expected error incrementing a non-integer value")
	}
}

func TestDBSizeAndFlushAll(t *testing.T) {
	s := newTestStore(4, 1<<20)
	s.Set("a", []byte("1"), SetOptions{})
	s.Set("b", []byte("2"), SetOptions{})
	if s.DBSize() != 2 {
		t.Fatalf("expected DBSize 2, got %d", s.DBSize())
	}
	s.FlushAll()
	if s.DBSize() != 0 {
		t.Fatalf("expected DBSize 0 after FlushAll, got %d", s.DBSize())
	}
}

func TestForEachVisitsLiveEntriesOnly(t *testing.T) {
	s := newTestStore(4, 1<<20)
	s.Set("live", []byte("v"), SetOptions{})
	s.Set("dead", []byte("v"), SetOptions{TTL: 10 * time.Millisecond, HasTTL: true})
	time.Sleep(20 * time.Millisecond)

	seen := map[string]bool{}
	s.ForEach(func(key string, ent *CacheEntry) { seen[key] = true })

	if !seen["live"] {
		t.Fatal("expected 'live' to be visited")
	}
	if seen["dead"] {
		t.Fatal("expected expired 'dead' to be skipped")
	}
}

func TestRestoreEntryDropsAlreadyExpired(t *testing.T) {
	s := newTestStore(4, 1<<20)
	s.RestoreEntry("dead", &CacheEntry{
		Value:     []byte("v"),
		HasExpiry: true,
		ExpiresAt: time.Now().Add(-time.Second),
		SizeBytes: 10,
	})
	if _, ok := s.Get("dead"); ok {
		t.Fatal("expected an already-expired restored entry to be dropped")
	}

	s.RestoreEntry("live", &CacheEntry{Value: []byte("v"), SizeBytes: 10})
	if _, ok := s.Get("live"); !ok {
		t.Fatal("expected a live restored entry to be present")
	}
}

// ── LRU / eviction ────────────────────────────────────────────────────────────

func TestEvictionUnderMemoryPressure(t *testing.T) {
	// One shard, a tiny budget -- easy to force eviction deterministically.
	s := newTestStore(1, 200)

	s.Set("a", []byte("aaaaaaaaaa"), SetOptions{}) // ~10 bytes + overhead
	s.Set("b", []byte("bbbbbbbbbb"), SetOptions{})
	s.Get("a") // touch "a" so it's more recently used than "b"
	s.Set("c", []byte("cccccccccc"), SetOptions{})
	s.Set("d", []byte("dddddddddd"), SetOptions{})
	s.Set("e", []byte("eeeeeeeeee"), SetOptions{})

	// With a 200-byte budget and ~74 bytes/entry (10+64), at most 2 entries
	// fit. The least-recently-used ones should have been evicted.
	if s.DBSize() > 2 {
		t.Fatalf("expected eviction to keep DBSize small, got %d", s.DBSize())
	}
	if _, ok := s.Get("e"); !ok {
		t.Fatal("expected the most recently set key to survive eviction")
	}
}

func TestLRUOrderingSurvivesTouch(t *testing.T) {
	// Budget fits exactly 2 of these ~74-byte entries; inserting a 3rd
	// forces exactly one eviction, isolating what a single Get-driven
	// touch does to recency order.
	s := newTestStore(1, 160)

	s.Set("a", []byte("1234567890"), SetOptions{})
	s.Set("b", []byte("1234567890"), SetOptions{})
	s.Get("a") // "a" is now more recently used than "b"
	s.Set("c", []byte("1234567890"), SetOptions{}) // triggers eviction: "b" is LRU, "a" isn't

	if s.Exists("b") {
		t.Fatal("expected 'b' (least recently used) to be evicted")
	}
	if !s.Exists("a") {
		t.Fatal("expected 'a' (recently touched) to survive")
	}
}

// ── Active expiration worker ──────────────────────────────────────────────────

func TestActiveExpirationWorkerSweepsExpiredKeys(t *testing.T) {
	s := newTestStore(2, 1<<20)
	s.Set("short", []byte("v"), SetOptions{TTL: 10 * time.Millisecond, HasTTL: true})

	// Directly sweep rather than waiting on a real ticker, keeping the test
	// fast and deterministic.
	time.Sleep(20 * time.Millisecond)
	for _, sh := range s.shards {
		sh.sweepExpired()
	}

	if s.DBSize() != 0 {
		t.Fatalf("expected expired key removed by sweep, DBSize=%d", s.DBSize())
	}
}

// ── Concurrency ───────────────────────────────────────────────────────────────

// Many goroutines hammering overlapping keys across all shards -- this is
// what sharding is for, and it's the test -race is meant to catch problems
// in (a shared LRU list or byte counter touched without its shard's lock).
func TestConcurrentAccessAcrossShards(t *testing.T) {
	s := newTestStore(16, 10<<20)

	const goroutines = 32
	const opsPerGoroutine = 500

	var wg sync.WaitGroup
	wg.Add(goroutines)
	for g := 0; g < goroutines; g++ {
		go func(id int) {
			defer wg.Done()
			for i := 0; i < opsPerGoroutine; i++ {
				key := fmt.Sprintf("key:%d:%d", id%4, i%8) // deliberate cross-goroutine overlap
				switch i % 5 {
				case 0:
					s.Set(key, []byte("v"), SetOptions{})
				case 1:
					s.Get(key)
				case 2:
					s.Delete(key)
				case 3:
					s.IncrBy("counter:"+key, 1)
				case 4:
					s.Expire(key, time.Minute)
				}
			}
		}(g)
	}
	wg.Wait()
}
