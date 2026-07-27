// Package persistence implements snapshot save/load: serializing the live
// contents of a storage.ShardedStore to a binary file and restoring it on
// startup, so the cache survives a restart.
package persistence

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"hash/crc32"
	"io"
	"os"
	"path/filepath"
	"time"

	"sqlopt/cache/internal/storage"
)

// File format:
//
//	magic bytes:    "GOCACHE1"   (8 bytes)
//	format version: uint32       (big-endian)
//	entry count:    uint64
//	for each entry:
//	  key length:    uint32
//	  key bytes
//	  value length:  uint32
//	  value bytes
//	  has_expiry:    byte (0 or 1)
//	  expires_at:    int64  (Unix nanoseconds; meaningful only if has_expiry=1)
//	checksum:        uint32  (CRC32-IEEE of every byte before it)
const (
	magic         = "GOCACHE1"
	formatVersion = uint32(1)

	// Upper bound on a single key/value length field, checked before
	// allocating. A corrupt or truncated snapshot file can otherwise put
	// an arbitrary uint32 in a length prefix and make LoadSnapshot try to
	// allocate up to 4GB per field -- the same class of bug the RESP
	// parser had to guard against for network input.
	maxFieldLength = 512 * 1024 * 1024 // 512 MiB
)

// SaveSnapshot writes every live entry in store to path. It builds the
// whole payload in memory first (this cache's size is memory-bounded by
// config, so that's safe), then writes it to a temp file in the same
// directory, fsyncs, and atomically renames over the destination -- a
// crash or power loss mid-write can never leave a corrupt/partial
// snapshot at the real path.
func SaveSnapshot(store *storage.ShardedStore, path string) error {
	var buf bytes.Buffer

	buf.WriteString(magic)
	_ = binary.Write(&buf, binary.BigEndian, formatVersion)

	// Collect first so we can write an accurate entry count before the
	// entries themselves (ForEach doesn't know the count up front).
	type kv struct {
		key string
		ent *storage.CacheEntry
	}
	var entries []kv
	store.ForEach(func(key string, ent *storage.CacheEntry) {
		entries = append(entries, kv{key, ent})
	})

	_ = binary.Write(&buf, binary.BigEndian, uint64(len(entries)))

	for _, e := range entries {
		writeBytesField(&buf, []byte(e.key))
		writeBytesField(&buf, e.ent.Value)

		if e.ent.HasExpiry {
			buf.WriteByte(1)
			_ = binary.Write(&buf, binary.BigEndian, e.ent.ExpiresAt.UnixNano())
		} else {
			buf.WriteByte(0)
			_ = binary.Write(&buf, binary.BigEndian, int64(0))
		}
	}

	checksum := crc32.ChecksumIEEE(buf.Bytes())
	_ = binary.Write(&buf, binary.BigEndian, checksum)

	dir := filepath.Dir(path)
	tmp, err := os.CreateTemp(dir, filepath.Base(path)+".tmp-*")
	if err != nil {
		return fmt.Errorf("persistence: create temp file: %w", err)
	}
	tmpName := tmp.Name()
	defer os.Remove(tmpName) // no-op once the rename below succeeds

	if _, err := tmp.Write(buf.Bytes()); err != nil {
		tmp.Close()
		return fmt.Errorf("persistence: write temp file: %w", err)
	}
	if err := tmp.Sync(); err != nil {
		tmp.Close()
		return fmt.Errorf("persistence: fsync temp file: %w", err)
	}
	if err := tmp.Close(); err != nil {
		return fmt.Errorf("persistence: close temp file: %w", err)
	}
	if err := os.Rename(tmpName, path); err != nil {
		return fmt.Errorf("persistence: rename into place: %w", err)
	}
	return nil
}

// LoadSnapshot reads path and restores every entry into store. Entries
// that had already expired by the time the snapshot was taken are simply
// dropped (storage.RestoreEntry does this). A missing file is not an
// error -- it just means there's nothing to restore yet.
func LoadSnapshot(store *storage.ShardedStore, path string) error {
	data, err := os.ReadFile(path)
	if err != nil {
		if os.IsNotExist(err) {
			return nil
		}
		return fmt.Errorf("persistence: read snapshot: %w", err)
	}

	if len(data) < len(magic)+4+8+4 {
		return fmt.Errorf("persistence: snapshot file too short to be valid")
	}

	payload := data[:len(data)-4]
	wantChecksum := binary.BigEndian.Uint32(data[len(data)-4:])
	if crc32.ChecksumIEEE(payload) != wantChecksum {
		return fmt.Errorf("persistence: checksum mismatch -- snapshot file is corrupt")
	}

	r := bytes.NewReader(data)

	magicBuf := make([]byte, len(magic))
	if _, err := io.ReadFull(r, magicBuf); err != nil || string(magicBuf) != magic {
		return fmt.Errorf("persistence: bad magic bytes -- not a valid snapshot")
	}

	var version uint32
	if err := binary.Read(r, binary.BigEndian, &version); err != nil {
		return fmt.Errorf("persistence: read version: %w", err)
	}
	if version != formatVersion {
		return fmt.Errorf("persistence: unsupported snapshot format version %d", version)
	}

	var count uint64
	if err := binary.Read(r, binary.BigEndian, &count); err != nil {
		return fmt.Errorf("persistence: read entry count: %w", err)
	}

	for i := uint64(0); i < count; i++ {
		key, err := readBytesField(r)
		if err != nil {
			return fmt.Errorf("persistence: read key %d: %w", i, err)
		}
		value, err := readBytesField(r)
		if err != nil {
			return fmt.Errorf("persistence: read value %d: %w", i, err)
		}

		hasExpiryByte := make([]byte, 1)
		if _, err := io.ReadFull(r, hasExpiryByte); err != nil {
			return fmt.Errorf("persistence: read expiry flag %d: %w", i, err)
		}
		var expiresAtNano int64
		if err := binary.Read(r, binary.BigEndian, &expiresAtNano); err != nil {
			return fmt.Errorf("persistence: read expiry timestamp %d: %w", i, err)
		}

		ent := &storage.CacheEntry{Value: value}
		if hasExpiryByte[0] == 1 {
			ent.HasExpiry = true
			ent.ExpiresAt = time.Unix(0, expiresAtNano)
		}
		store.RestoreEntry(string(key), ent)
	}

	return nil
}

func writeBytesField(buf *bytes.Buffer, b []byte) {
	_ = binary.Write(buf, binary.BigEndian, uint32(len(b)))
	buf.Write(b)
}

func readBytesField(r *bytes.Reader) ([]byte, error) {
	var length uint32
	if err := binary.Read(r, binary.BigEndian, &length); err != nil {
		return nil, err
	}
	if length > maxFieldLength {
		return nil, fmt.Errorf("field length %d exceeds max of %d -- snapshot is likely corrupt", length, maxFieldLength)
	}
	buf := make([]byte, length)
	if _, err := io.ReadFull(r, buf); err != nil {
		return nil, err
	}
	return buf, nil
}
