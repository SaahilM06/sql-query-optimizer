package main

import (
	"bufio"
	"encoding/json"
	"os"
	"path/filepath"
	"testing"
	"time"
)

func TestAppendResultCreatesFileAndDir(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "nested", "results.jsonl")

	r := BenchmarkResult{Timestamp: time.Now().UTC(), Clients: 4, Requests: 100, ThroughputOpsPerSec: 1234.5}
	if err := appendResult(path, r); err != nil {
		t.Fatalf("appendResult: %v", err)
	}

	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("ReadFile: %v", err)
	}
	var got BenchmarkResult
	if err := json.Unmarshal(data[:len(data)-1], &got); err != nil { // strip trailing newline
		t.Fatalf("Unmarshal: %v", err)
	}
	if got.Clients != 4 || got.Requests != 100 || got.ThroughputOpsPerSec != 1234.5 {
		t.Fatalf("got %+v", got)
	}
}

func TestAppendResultAppendsMultipleLines(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "results.jsonl")

	for i := 0; i < 3; i++ {
		r := BenchmarkResult{Timestamp: time.Now().UTC(), Clients: i + 1}
		if err := appendResult(path, r); err != nil {
			t.Fatalf("appendResult run %d: %v", i, err)
		}
	}

	f, err := os.Open(path)
	if err != nil {
		t.Fatalf("Open: %v", err)
	}
	defer f.Close()

	var lines []string
	scanner := bufio.NewScanner(f)
	for scanner.Scan() {
		lines = append(lines, scanner.Text())
	}
	if len(lines) != 3 {
		t.Fatalf("expected 3 appended lines, got %d", len(lines))
	}
	for i, line := range lines {
		var r BenchmarkResult
		if err := json.Unmarshal([]byte(line), &r); err != nil {
			t.Fatalf("line %d not valid JSON: %v", i, err)
		}
		if r.Clients != i+1 {
			t.Fatalf("line %d: expected Clients=%d, got %d", i, i+1, r.Clients)
		}
	}
}
