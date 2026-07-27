// Command benchmark drives concurrent load against a running plan-cache
// server and reports throughput and latency percentiles.
//
// Usage:
//
//	go run ./cmd/benchmark \
//	  -address localhost:6380 \
//	  -clients 32 \
//	  -requests 100000 \
//	  -ratio-get 0.8
package main

import (
	"bufio"
	"encoding/json"
	"flag"
	"fmt"
	"math/rand"
	"net"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	"sqlopt/cache/internal/resp"
)

func main() {
	address := flag.String("address", "localhost:6380", "server address")
	clients := flag.Int("clients", 32, "number of concurrent client connections")
	requests := flag.Int("requests", 100_000, "total number of requests across all clients")
	ratioGet := flag.Float64("ratio-get", 0.8, "fraction of requests that are GET (the rest are SET)")
	keyspace := flag.Int("keyspace", 10_000, "number of distinct keys used")
	valueSize := flag.Int("value-size", 100, "size in bytes of values written by SET")
	label := flag.String("label", "", "optional free-text label for this run, stored alongside the results (e.g. a git commit or scenario name)")
	output := flag.String("output", "benchmarks/results.jsonl", "append a JSON record of this run's results to this file (one JSON object per line); empty disables")
	flag.Parse()

	if *clients < 1 {
		fmt.Fprintln(os.Stderr, "clients must be >= 1")
		os.Exit(1)
	}
	if *requests < *clients {
		fmt.Fprintln(os.Stderr, "requests must be >= clients")
		os.Exit(1)
	}

	opsPerClient := *requests / *clients
	totalOps := opsPerClient * *clients
	value := strings.Repeat("x", *valueSize)

	var wg sync.WaitGroup
	var getCount, setCount, errorCount atomic.Int64
	perClientLatencies := make([][]time.Duration, *clients)

	start := time.Now()
	for c := 0; c < *clients; c++ {
		wg.Add(1)
		go func(id int) {
			defer wg.Done()

			conn, err := net.Dial("tcp", *address)
			if err != nil {
				fmt.Fprintf(os.Stderr, "client %d: dial failed: %v\n", id, err)
				errorCount.Add(int64(opsPerClient))
				return
			}
			defer conn.Close()

			reader := bufio.NewReader(conn)
			writer := bufio.NewWriter(conn)
			rng := rand.New(rand.NewSource(time.Now().UnixNano() + int64(id)))

			local := make([]time.Duration, 0, opsPerClient)
			for i := 0; i < opsPerClient; i++ {
				key := "bench:" + strconv.Itoa(rng.Intn(*keyspace))
				isGet := rng.Float64() < *ratioGet

				var req resp.RespValue
				if isGet {
					req = resp.EncodeCommand("GET", key)
				} else {
					req = resp.EncodeCommand("SET", key, value)
				}

				opStart := time.Now()
				if err := resp.WriteRESP(writer, req); err != nil {
					errorCount.Add(1)
					return
				}
				if err := writer.Flush(); err != nil {
					errorCount.Add(1)
					return
				}
				if _, err := resp.ParseRESP(reader); err != nil {
					errorCount.Add(1)
					return
				}
				local = append(local, time.Since(opStart))

				if isGet {
					getCount.Add(1)
				} else {
					setCount.Add(1)
				}
			}
			perClientLatencies[id] = local
		}(c)
	}
	wg.Wait()
	elapsed := time.Since(start)

	var all []time.Duration
	for _, l := range perClientLatencies {
		all = append(all, l...)
	}
	sort.Slice(all, func(i, j int) bool { return all[i] < all[j] })

	throughput := float64(len(all)) / elapsed.Seconds()

	stats, infoErr := fetchInfoStats(*address)

	fmt.Println()
	fmt.Printf("Clients:        %d\n", *clients)
	fmt.Printf("Requests:       %s\n", commaInt(totalOps))
	fmt.Printf("Workload:       %.0f%% GET / %.0f%% SET\n", *ratioGet*100, (1-*ratioGet)*100)
	fmt.Printf("Throughput:     %s ops/sec\n", commaFloat(throughput))
	fmt.Printf("p50 latency:    %.2f ms\n", ms(percentile(all, 0.50)))
	fmt.Printf("p95 latency:    %.2f ms\n", ms(percentile(all, 0.95)))
	fmt.Printf("p99 latency:    %.2f ms\n", ms(percentile(all, 0.99)))
	if infoErr == nil {
		fmt.Printf("Hit rate:       %.1f%%\n", stats.hitRate*100)
		fmt.Printf("Evicted keys:   %s\n", commaInt(stats.evictedKeys))
		fmt.Printf("Expired keys:   %s\n", commaInt(stats.expiredKeys))
		fmt.Printf("Memory used:    %s bytes\n", commaInt(stats.usedMemoryBytes))
	} else {
		fmt.Printf("(could not fetch INFO for hit-rate/eviction stats: %v)\n", infoErr)
	}
	if errorCount.Load() > 0 {
		fmt.Printf("Errors:         %d\n", errorCount.Load())
	}

	if *output != "" {
		result := BenchmarkResult{
			Timestamp:           time.Now().UTC(),
			Label:               *label,
			Address:             *address,
			Clients:             *clients,
			Requests:            totalOps,
			RatioGet:            *ratioGet,
			Keyspace:            *keyspace,
			ValueSizeBytes:      *valueSize,
			ThroughputOpsPerSec: throughput,
			P50Ms:               ms(percentile(all, 0.50)),
			P95Ms:               ms(percentile(all, 0.95)),
			P99Ms:               ms(percentile(all, 0.99)),
			Errors:              errorCount.Load(),
		}
		if infoErr == nil {
			result.HitRate = stats.hitRate
			result.EvictedKeys = stats.evictedKeys
			result.ExpiredKeys = stats.expiredKeys
			result.MemoryUsedBytes = stats.usedMemoryBytes
		}
		if err := appendResult(*output, result); err != nil {
			fmt.Fprintf(os.Stderr, "warning: could not write results to %s: %v\n", *output, err)
		} else {
			fmt.Printf("\nResult appended to %s\n", *output)
		}
	}
}

// BenchmarkResult is one run's worth of measurements, structured for
// machine-readable storage rather than just printed to stdout -- so
// results can be tracked over time, diffed across commits, or plotted,
// instead of only living in a terminal scrollback or a hand-copied README
// table.
type BenchmarkResult struct {
	Timestamp time.Time `json:"timestamp"`
	Label     string    `json:"label,omitempty"`
	Address   string    `json:"address"`

	Clients        int     `json:"clients"`
	Requests       int     `json:"requests"`
	RatioGet       float64 `json:"ratio_get"`
	Keyspace       int     `json:"keyspace"`
	ValueSizeBytes int     `json:"value_size_bytes"`

	ThroughputOpsPerSec float64 `json:"throughput_ops_per_sec"`
	P50Ms               float64 `json:"p50_ms"`
	P95Ms               float64 `json:"p95_ms"`
	P99Ms               float64 `json:"p99_ms"`

	HitRate         float64 `json:"hit_rate"`
	EvictedKeys     int     `json:"evicted_keys"`
	ExpiredKeys     int     `json:"expired_keys"`
	MemoryUsedBytes int     `json:"memory_used_bytes"`

	Errors int64 `json:"errors"`
}

// appendResult writes result as one JSON line appended to path (creating
// the file and any parent directory if needed). JSON Lines rather than a
// single JSON array so concurrent/repeated runs can never corrupt earlier
// entries -- each append is independent.
func appendResult(path string, result BenchmarkResult) error {
	if dir := filepath.Dir(path); dir != "." {
		if err := os.MkdirAll(dir, 0o755); err != nil {
			return fmt.Errorf("create output directory: %w", err)
		}
	}

	f, err := os.OpenFile(path, os.O_APPEND|os.O_CREATE|os.O_WRONLY, 0o644)
	if err != nil {
		return fmt.Errorf("open output file: %w", err)
	}
	defer f.Close()

	data, err := json.Marshal(result)
	if err != nil {
		return fmt.Errorf("marshal result: %w", err)
	}
	if _, err := f.Write(append(data, '\n')); err != nil {
		return fmt.Errorf("write result: %w", err)
	}
	return nil
}

func percentile(sorted []time.Duration, p float64) time.Duration {
	if len(sorted) == 0 {
		return 0
	}
	idx := int(p * float64(len(sorted)))
	if idx >= len(sorted) {
		idx = len(sorted) - 1
	}
	return sorted[idx]
}

func ms(d time.Duration) float64 {
	return float64(d.Microseconds()) / 1000.0
}

type infoStats struct {
	hitRate         float64
	evictedKeys     int
	expiredKeys     int
	usedMemoryBytes int
}

// fetchInfoStats opens a short-lived connection, runs INFO, and pulls the
// handful of fields the benchmark report cares about out of its plain-text
// body.
func fetchInfoStats(address string) (infoStats, error) {
	conn, err := net.Dial("tcp", address)
	if err != nil {
		return infoStats{}, err
	}
	defer conn.Close()

	writer := bufio.NewWriter(conn)
	if err := resp.WriteRESP(writer, resp.EncodeCommand("INFO")); err != nil {
		return infoStats{}, err
	}
	if err := writer.Flush(); err != nil {
		return infoStats{}, err
	}

	reply, err := resp.ParseRESP(bufio.NewReader(conn))
	if err != nil {
		return infoStats{}, err
	}
	if reply.Type != resp.RespBulkString {
		return infoStats{}, fmt.Errorf("unexpected INFO reply type %d", reply.Type)
	}

	var stats infoStats
	for _, line := range strings.Split(string(reply.Bulk), "\r\n") {
		key, val, ok := strings.Cut(line, ":")
		if !ok {
			continue
		}
		switch key {
		case "hit_rate":
			stats.hitRate, _ = strconv.ParseFloat(val, 64)
		case "evicted_keys":
			stats.evictedKeys, _ = strconv.Atoi(val)
		case "expired_keys":
			stats.expiredKeys, _ = strconv.Atoi(val)
		case "used_memory_bytes":
			stats.usedMemoryBytes, _ = strconv.Atoi(val)
		}
	}
	return stats, nil
}

func commaInt(n int) string {
	return commaFloat(float64(n))
}

// commaFloat formats n with thousands separators and, for non-integer
// values, two decimal places (e.g. 74212.7 -> "74,212.70").
func commaFloat(n float64) string {
	neg := n < 0
	if neg {
		n = -n
	}
	whole := int64(n)
	frac := n - float64(whole)

	s := strconv.FormatInt(whole, 10)
	var b strings.Builder
	for i, c := range s {
		if i > 0 && (len(s)-i)%3 == 0 {
			b.WriteByte(',')
		}
		b.WriteRune(c)
	}
	out := b.String()
	if frac > 0.001 {
		out += strings.TrimPrefix(fmt.Sprintf("%.2f", frac), "0")
	}
	if neg {
		out = "-" + out
	}
	return out
}
