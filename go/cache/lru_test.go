package cache

import (
	"testing"
	"time"
)

func TestGetSetRoundTrip(t *testing.T) {
	c := New(10)
	defer c.Close()

	c.Set("plan:1", []byte("scan(orders)"), 0)
	got, ok := c.Get("plan:1")
	if !ok {
		t.Fatal("expected key to be present")
	}
	if string(got) != "scan(orders)" {
		t.Fatalf("got %q, want %q", got, "scan(orders)")
	}
}

func TestGetMissingKey(t *testing.T) {
	c := New(10)
	defer c.Close()

	if _, ok := c.Get("nope"); ok {
		t.Fatal("expected miss for absent key")
	}
}

func TestLRUEvictsLeastRecentlyUsed(t *testing.T) {
	c := New(2)
	defer c.Close()

	c.Set("a", []byte("1"), 0)
	c.Set("b", []byte("2"), 0)
	// Touch "a" so "b" becomes the least-recently-used entry.
	c.Get("a")
	c.Set("c", []byte("3"), 0) // should evict "b", not "a"

	if _, ok := c.Get("b"); ok {
		t.Fatal("expected 'b' to be evicted as least-recently-used")
	}
	if _, ok := c.Get("a"); !ok {
		t.Fatal("expected 'a' to survive eviction (recently touched)")
	}
	if _, ok := c.Get("c"); !ok {
		t.Fatal("expected 'c' to be present (just inserted)")
	}
	if c.Len() != 2 {
		t.Fatalf("expected capacity-bounded length 2, got %d", c.Len())
	}
}

func TestSetOverwriteRefreshesRecency(t *testing.T) {
	c := New(2)
	defer c.Close()

	c.Set("a", []byte("1"), 0)
	c.Set("b", []byte("2"), 0)
	c.Set("a", []byte("1-updated"), 0) // overwrite -- "a" becomes most-recently-used
	c.Set("c", []byte("3"), 0)         // should evict "b"

	if _, ok := c.Get("b"); ok {
		t.Fatal("expected 'b' to be evicted")
	}
	val, ok := c.Get("a")
	if !ok || string(val) != "1-updated" {
		t.Fatalf("expected 'a' updated value to survive, got %q ok=%v", val, ok)
	}
}

func TestTTLExpiresEntry(t *testing.T) {
	c := New(10)
	defer c.Close()

	c.Set("short", []byte("v"), 20*time.Millisecond)
	if _, ok := c.Get("short"); !ok {
		t.Fatal("expected key present before expiry")
	}

	time.Sleep(40 * time.Millisecond)

	if _, ok := c.Get("short"); ok {
		t.Fatal("expected key to be expired")
	}
}

func TestTTLReporting(t *testing.T) {
	c := New(10)
	defer c.Close()

	c.Set("no-expiry", []byte("v"), 0)
	ttl, ok := c.TTL("no-expiry")
	if !ok {
		t.Fatal("expected key to exist")
	}
	if ttl != 0 {
		t.Fatalf("expected zero TTL (no expiration), got %v", ttl)
	}

	c.Set("expiring", []byte("v"), 5*time.Second)
	ttl, ok = c.TTL("expiring")
	if !ok {
		t.Fatal("expected key to exist")
	}
	if ttl <= 0 || ttl > 5*time.Second {
		t.Fatalf("expected TTL in (0, 5s], got %v", ttl)
	}

	if _, ok := c.TTL("missing"); ok {
		t.Fatal("expected TTL lookup on missing key to report absent")
	}
}

func TestDelete(t *testing.T) {
	c := New(10)
	defer c.Close()

	c.Set("a", []byte("1"), 0)
	if !c.Delete("a") {
		t.Fatal("expected Delete to report the key was present")
	}
	if c.Delete("a") {
		t.Fatal("expected second Delete of the same key to report absent")
	}
	if _, ok := c.Get("a"); ok {
		t.Fatal("expected key to be gone after Delete")
	}
}

func TestBackgroundJanitorSweepsExpiredEntries(t *testing.T) {
	c := New(10)
	defer c.Close()

	c.Set("short", []byte("v"), 10*time.Millisecond)
	// Give the 1s janitor tick no chance to fire; instead force a sweep
	// directly to keep the test fast and deterministic.
	time.Sleep(20 * time.Millisecond)
	c.sweepExpired()

	if c.Len() != 0 {
		t.Fatalf("expected janitor to remove expired entry, len=%d", c.Len())
	}
}
