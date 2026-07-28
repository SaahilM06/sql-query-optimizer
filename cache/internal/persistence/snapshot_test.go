package persistence

import (
	"os"
	"path/filepath"
	"testing"
	"time"

	"sqlopt/cache/internal/metrics"
	"sqlopt/cache/internal/storage"
)

func newTestStore() *storage.ShardedStore {
	return storage.New(4, 1<<20, metrics.New())
}

func TestSaveAndLoadRoundTrip(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "snap.bin")

	s := newTestStore()
	s.Set("a", []byte("1"), storage.SetOptions{})
	s.Set("b", []byte("hello world"), storage.SetOptions{TTL: time.Hour, HasTTL: true})
	s.Set("binary", []byte{0x00, 0x01, 0xff, '\r', '\n'}, storage.SetOptions{})

	if err := SaveSnapshot(s, path); err != nil {
		t.Fatalf("SaveSnapshot: %v", err)
	}

	restored := newTestStore()
	if err := LoadSnapshot(restored, path); err != nil {
		t.Fatalf("LoadSnapshot: %v", err)
	}

	if v, ok := restored.Get("a"); !ok || string(v) != "1" {
		t.Fatalf("key 'a': got %q ok=%v", v, ok)
	}
	if v, ok := restored.Get("b"); !ok || string(v) != "hello world" {
		t.Fatalf("key 'b': got %q ok=%v", v, ok)
	}
	if ttl, ok := restored.TTL("b"); !ok || ttl <= 0 || ttl > time.Hour {
		t.Fatalf("key 'b' TTL not preserved: %v ok=%v", ttl, ok)
	}
	if v, ok := restored.Get("binary"); !ok || len(v) != 5 {
		t.Fatalf("binary key mangled: %v ok=%v", v, ok)
	}
}

func TestLoadMissingFileIsNotAnError(t *testing.T) {
	dir := t.TempDir()
	s := newTestStore()
	if err := LoadSnapshot(s, filepath.Join(dir, "does-not-exist.bin")); err != nil {
		t.Fatalf("expected no error loading a missing snapshot, got %v", err)
	}
	if s.DBSize() != 0 {
		t.Fatal("expected empty store after loading a missing snapshot")
	}
}

func TestAlreadyExpiredEntryIsDroppedOnLoad(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "snap.bin")

	s := newTestStore()
	s.Set("soon-dead", []byte("v"), storage.SetOptions{TTL: 10 * time.Millisecond, HasTTL: true})
	if err := SaveSnapshot(s, path); err != nil {
		t.Fatalf("SaveSnapshot: %v", err)
	}

	time.Sleep(30 * time.Millisecond) // let it expire before loading

	restored := newTestStore()
	if err := LoadSnapshot(restored, path); err != nil {
		t.Fatalf("LoadSnapshot: %v", err)
	}
	if _, ok := restored.Get("soon-dead"); ok {
		t.Fatal("expected an already-expired snapshot entry to be dropped on load")
	}
}

func TestCorruptedChecksumRejected(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "snap.bin")

	s := newTestStore()
	s.Set("a", []byte("1"), storage.SetOptions{})
	if err := SaveSnapshot(s, path); err != nil {
		t.Fatalf("SaveSnapshot: %v", err)
	}

	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("ReadFile: %v", err)
	}
	data[len(data)/2] ^= 0xFF // flip a bit in the middle of the payload
	if err := os.WriteFile(path, data, 0o644); err != nil {
		t.Fatalf("WriteFile: %v", err)
	}

	restored := newTestStore()
	if err := LoadSnapshot(restored, path); err == nil {
		t.Fatal("expected checksum mismatch to be rejected")
	}
}

func TestSaveIsAtomic_NoPartialFileOnDestination(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "snap.bin")

	s := newTestStore()
	s.Set("a", []byte("1"), storage.SetOptions{})
	if err := SaveSnapshot(s, path); err != nil {
		t.Fatalf("first save: %v", err)
	}

	// A second save should cleanly replace the file via rename, leaving no
	// leftover .tmp-* files behind in the directory.
	s.Set("b", []byte("2"), storage.SetOptions{})
	if err := SaveSnapshot(s, path); err != nil {
		t.Fatalf("second save: %v", err)
	}

	entries, err := os.ReadDir(dir)
	if err != nil {
		t.Fatalf("ReadDir: %v", err)
	}
	if len(entries) != 1 || entries[0].Name() != "snap.bin" {
		t.Fatalf("expected exactly one file (snap.bin), got %v", entries)
	}
}
