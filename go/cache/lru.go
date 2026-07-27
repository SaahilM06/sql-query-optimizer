// Package cache implements a Redis-compatible in-memory cache for storing
// optimized SQL execution plans, so repeated queries skip re-optimization.
// It provides an LRU-evicting, TTL-expiring key/value store plus a RESP
// (REdis Serialization Protocol) TCP server for talking to it.
package cache

import (
	"container/list"
	"sync"
	"time"
)

type entry struct {
	key       string
	value     []byte
	expiresAt time.Time // zero value means "no expiration"
}

// Cache is a thread-safe, fixed-capacity LRU cache with per-key TTL
// expiration. Eviction is O(1): a doubly linked list tracks recency
// (front = most recently used, back = least recently used) alongside a
// map for O(1) lookup.
type Cache struct {
	mu       sync.Mutex
	capacity int
	ll       *list.List
	items    map[string]*list.Element

	stopJanitor chan struct{}
}

// New creates a cache that holds at most `capacity` entries, evicting the
// least-recently-used entry once that limit is exceeded. A background
// janitor sweeps expired entries every second so TTLs are enforced even
// for keys that are never touched again.
func New(capacity int) *Cache {
	if capacity <= 0 {
		capacity = 1
	}
	c := &Cache{
		capacity:    capacity,
		ll:          list.New(),
		items:       make(map[string]*list.Element),
		stopJanitor: make(chan struct{}),
	}
	go c.runJanitor(time.Second)
	return c
}

// Close stops the background expiration sweep. Safe to call once.
func (c *Cache) Close() {
	close(c.stopJanitor)
}

func (c *Cache) runJanitor(interval time.Duration) {
	ticker := time.NewTicker(interval)
	defer ticker.Stop()
	for {
		select {
		case <-ticker.C:
			c.sweepExpired()
		case <-c.stopJanitor:
			return
		}
	}
}

func (c *Cache) sweepExpired() {
	c.mu.Lock()
	defer c.mu.Unlock()
	now := time.Now()
	for e := c.ll.Back(); e != nil; {
		prev := e.Prev()
		ent := e.Value.(*entry)
		if !ent.expiresAt.IsZero() && now.After(ent.expiresAt) {
			c.removeElement(e)
		}
		e = prev
	}
}

// Get returns the value for key and marks it most-recently-used. ok is
// false if the key is absent or has expired.
func (c *Cache) Get(key string) (value []byte, ok bool) {
	c.mu.Lock()
	defer c.mu.Unlock()

	el, found := c.items[key]
	if !found {
		return nil, false
	}
	ent := el.Value.(*entry)
	if c.isExpired(ent) {
		c.removeElement(el)
		return nil, false
	}
	c.ll.MoveToFront(el)
	return ent.value, true
}

// Set inserts or updates key, marking it most-recently-used. ttl of 0
// means the entry never expires. If the cache is at capacity and key is
// new, the least-recently-used entry is evicted.
func (c *Cache) Set(key string, value []byte, ttl time.Duration) {
	c.mu.Lock()
	defer c.mu.Unlock()

	var expiresAt time.Time
	if ttl > 0 {
		expiresAt = time.Now().Add(ttl)
	}

	if el, found := c.items[key]; found {
		ent := el.Value.(*entry)
		ent.value = value
		ent.expiresAt = expiresAt
		c.ll.MoveToFront(el)
		return
	}

	ent := &entry{key: key, value: value, expiresAt: expiresAt}
	el := c.ll.PushFront(ent)
	c.items[key] = el

	if c.ll.Len() > c.capacity {
		c.removeElement(c.ll.Back())
	}
}

// Delete removes key. Reports whether it was present.
func (c *Cache) Delete(key string) bool {
	c.mu.Lock()
	defer c.mu.Unlock()

	el, found := c.items[key]
	if !found {
		return false
	}
	c.removeElement(el)
	return true
}

// TTL returns the remaining time-to-live for key. ok is false if the key
// is absent or expired; a zero duration with ok true means the key exists
// but has no expiration set.
func (c *Cache) TTL(key string) (ttl time.Duration, ok bool) {
	c.mu.Lock()
	defer c.mu.Unlock()

	el, found := c.items[key]
	if !found {
		return 0, false
	}
	ent := el.Value.(*entry)
	if c.isExpired(ent) {
		c.removeElement(el)
		return 0, false
	}
	if ent.expiresAt.IsZero() {
		return 0, true
	}
	return time.Until(ent.expiresAt), true
}

// Len returns the number of live (non-expired) entries.
func (c *Cache) Len() int {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.ll.Len()
}

func (c *Cache) isExpired(ent *entry) bool {
	return !ent.expiresAt.IsZero() && time.Now().After(ent.expiresAt)
}

// removeElement unlinks el from both the list and the map. Caller must
// hold c.mu.
func (c *Cache) removeElement(el *list.Element) {
	c.ll.Remove(el)
	ent := el.Value.(*entry)
	delete(c.items, ent.key)
}
