// Package metrics tracks server-wide counters exposed through the INFO
// command. All fields are updated with atomics so any goroutine (a
// connection handler, the expiration worker, an eviction inside storage)
// can bump them without a lock.
package metrics

import (
	"sync/atomic"
	"time"
)

type Metrics struct {
	StartTime time.Time

	CommandsProcessed atomic.Uint64
	CacheHits         atomic.Uint64
	CacheMisses       atomic.Uint64
	ExpiredKeys       atomic.Uint64
	EvictedKeys       atomic.Uint64
	Connections       atomic.Uint64 // currently-open connections
	TotalConnections  atomic.Uint64 // lifetime accepted connections
	CurrentEntries    atomic.Int64
	MemoryBytes       atomic.Int64
}

func New() *Metrics {
	return &Metrics{StartTime: time.Now()}
}

func (m *Metrics) Uptime() time.Duration {
	return time.Since(m.StartTime)
}

// HitRate returns hits / (hits + misses) as a fraction in [0, 1], or 0 if
// there have been no lookups yet.
func (m *Metrics) HitRate() float64 {
	hits := m.CacheHits.Load()
	misses := m.CacheMisses.Load()
	total := hits + misses
	if total == 0 {
		return 0
	}
	return float64(hits) / float64(total)
}

// Snapshot is a point-in-time, non-atomic copy of every counter, suitable
// for formatting into an INFO reply or a JSON report.
type Snapshot struct {
	UptimeSeconds     float64
	CommandsProcessed uint64
	CacheHits         uint64
	CacheMisses       uint64
	HitRate           float64
	ExpiredKeys       uint64
	EvictedKeys       uint64
	Connections       uint64
	TotalConnections  uint64
	CurrentEntries    int64
	MemoryBytes       int64
}

func (m *Metrics) Snapshot() Snapshot {
	return Snapshot{
		UptimeSeconds:     m.Uptime().Seconds(),
		CommandsProcessed: m.CommandsProcessed.Load(),
		CacheHits:         m.CacheHits.Load(),
		CacheMisses:       m.CacheMisses.Load(),
		HitRate:           m.HitRate(),
		ExpiredKeys:       m.ExpiredKeys.Load(),
		EvictedKeys:       m.EvictedKeys.Load(),
		Connections:       m.Connections.Load(),
		TotalConnections:  m.TotalConnections.Load(),
		CurrentEntries:    m.CurrentEntries.Load(),
		MemoryBytes:       m.MemoryBytes.Load(),
	}
}
