package command

import (
	"strings"
	"testing"
	"time"

	"sqlopt/cache/internal/config"
	"sqlopt/cache/internal/metrics"
	"sqlopt/cache/internal/resp"
	"sqlopt/cache/internal/storage"
)

func newTestContext() *Context {
	cfg := config.Default()
	m := metrics.New()
	return &Context{
		Store:     storage.New(4, 1<<20, m),
		Metrics:   m,
		Config:    &cfg,
		StartTime: time.Now(),
	}
}

func run(t *testing.T, d *Dispatcher, ctx *Context, args ...string) resp.RespValue {
	t.Helper()
	return d.Dispatch(args, ctx)
}

func TestPing(t *testing.T) {
	d, ctx := NewDispatcher(), newTestContext()
	got := run(t, d, ctx, "PING")
	if got.Type != resp.RespSimpleString || got.String != "PONG" {
		t.Fatalf("got %+v", got)
	}
}

func TestPingWithMessage(t *testing.T) {
	d, ctx := NewDispatcher(), newTestContext()
	got := run(t, d, ctx, "PING", "hello")
	if got.Type != resp.RespBulkString || string(got.Bulk) != "hello" {
		t.Fatalf("got %+v", got)
	}
}

func TestEcho(t *testing.T) {
	d, ctx := NewDispatcher(), newTestContext()
	got := run(t, d, ctx, "ECHO", "hi")
	if got.Type != resp.RespBulkString || string(got.Bulk) != "hi" {
		t.Fatalf("got %+v", got)
	}
}

func TestSetAndGet(t *testing.T) {
	d, ctx := NewDispatcher(), newTestContext()
	run(t, d, ctx, "SET", "k", "v")
	got := run(t, d, ctx, "GET", "k")
	if got.Type != resp.RespBulkString || string(got.Bulk) != "v" {
		t.Fatalf("got %+v", got)
	}
}

func TestGetMissingReturnsNull(t *testing.T) {
	d, ctx := NewDispatcher(), newTestContext()
	got := run(t, d, ctx, "GET", "nope")
	if !got.IsNull() {
		t.Fatalf("expected null, got %+v", got)
	}
}

func TestSetWithEX(t *testing.T) {
	d, ctx := NewDispatcher(), newTestContext()
	setResp := run(t, d, ctx, "SET", "k", "v", "EX", "10")
	if setResp.Type != resp.RespSimpleString || setResp.String != "OK" {
		t.Fatalf("SET EX failed: %+v", setResp)
	}
	ttlResp := run(t, d, ctx, "TTL", "k")
	if ttlResp.Integer <= 0 || ttlResp.Integer > 10 {
		t.Fatalf("expected TTL in (0,10], got %+v", ttlResp)
	}
}

func TestSetWithPX(t *testing.T) {
	d, ctx := NewDispatcher(), newTestContext()
	run(t, d, ctx, "SET", "k", "v", "PX", "5000")
	ttlResp := run(t, d, ctx, "TTL", "k")
	if ttlResp.Integer <= 0 || ttlResp.Integer > 5 {
		t.Fatalf("expected TTL in (0,5], got %+v", ttlResp)
	}
}

func TestSetNX(t *testing.T) {
	d, ctx := NewDispatcher(), newTestContext()
	first := run(t, d, ctx, "SET", "k", "v1", "NX")
	if first.String != "OK" {
		t.Fatalf("expected first NX set to succeed, got %+v", first)
	}
	second := run(t, d, ctx, "SET", "k", "v2", "NX")
	if !second.IsNull() {
		t.Fatalf("expected second NX set to be rejected (null reply), got %+v", second)
	}
}

func TestSetXX(t *testing.T) {
	d, ctx := NewDispatcher(), newTestContext()
	miss := run(t, d, ctx, "SET", "k", "v", "XX")
	if !miss.IsNull() {
		t.Fatalf("expected XX set on missing key to be rejected, got %+v", miss)
	}
	run(t, d, ctx, "SET", "k", "v1")
	hit := run(t, d, ctx, "SET", "k", "v2", "XX")
	if hit.String != "OK" {
		t.Fatalf("expected XX set on existing key to succeed, got %+v", hit)
	}
}

func TestSetNXAndXXTogetherIsSyntaxError(t *testing.T) {
	d, ctx := NewDispatcher(), newTestContext()
	got := run(t, d, ctx, "SET", "k", "v", "NX", "XX")
	if got.Type != resp.RespError {
		t.Fatalf("expected syntax error, got %+v", got)
	}
}

func TestDelAndExists(t *testing.T) {
	d, ctx := NewDispatcher(), newTestContext()
	run(t, d, ctx, "SET", "a", "1")
	run(t, d, ctx, "SET", "b", "2")

	existsResp := run(t, d, ctx, "EXISTS", "a", "b", "missing")
	if existsResp.Integer != 2 {
		t.Fatalf("expected EXISTS=2, got %+v", existsResp)
	}
	delResp := run(t, d, ctx, "DEL", "a", "missing")
	if delResp.Integer != 1 {
		t.Fatalf("expected DEL=1, got %+v", delResp)
	}
}

func TestIncrDecr(t *testing.T) {
	d, ctx := NewDispatcher(), newTestContext()
	if got := run(t, d, ctx, "INCR", "counter"); got.Integer != 1 {
		t.Fatalf("got %+v", got)
	}
	if got := run(t, d, ctx, "INCR", "counter"); got.Integer != 2 {
		t.Fatalf("got %+v", got)
	}
	if got := run(t, d, ctx, "DECR", "counter"); got.Integer != 1 {
		t.Fatalf("got %+v", got)
	}
}

func TestIncrNonInteger(t *testing.T) {
	d, ctx := NewDispatcher(), newTestContext()
	run(t, d, ctx, "SET", "k", "not-a-number")
	got := run(t, d, ctx, "INCR", "k")
	if got.Type != resp.RespError {
		t.Fatalf("expected error, got %+v", got)
	}
}

func TestMSetMGet(t *testing.T) {
	d, ctx := NewDispatcher(), newTestContext()
	setResp := run(t, d, ctx, "MSET", "a", "1", "b", "2")
	if setResp.String != "OK" {
		t.Fatalf("MSET failed: %+v", setResp)
	}
	got := run(t, d, ctx, "MGET", "a", "b", "missing")
	if len(got.Array) != 3 {
		t.Fatalf("got %+v", got)
	}
	if string(got.Array[0].Bulk) != "1" || string(got.Array[1].Bulk) != "2" || !got.Array[2].IsNull() {
		t.Fatalf("element mismatch: %+v", got.Array)
	}
}

func TestMSetOddArgsIsError(t *testing.T) {
	d, ctx := NewDispatcher(), newTestContext()
	got := run(t, d, ctx, "MSET", "a", "1", "b")
	if got.Type != resp.RespError {
		t.Fatalf("expected error for odd arg count, got %+v", got)
	}
}

func TestType(t *testing.T) {
	d, ctx := NewDispatcher(), newTestContext()
	run(t, d, ctx, "SET", "k", "v")
	if got := run(t, d, ctx, "TYPE", "k"); got.String != "string" {
		t.Fatalf("got %+v", got)
	}
	if got := run(t, d, ctx, "TYPE", "missing"); got.String != "none" {
		t.Fatalf("got %+v", got)
	}
}

func TestExpirePersistTTL(t *testing.T) {
	d, ctx := NewDispatcher(), newTestContext()
	run(t, d, ctx, "SET", "k", "v")

	if got := run(t, d, ctx, "TTL", "k"); got.Integer != -1 {
		t.Fatalf("expected -1 (no TTL), got %+v", got)
	}
	if got := run(t, d, ctx, "TTL", "missing"); got.Integer != -2 {
		t.Fatalf("expected -2 (missing), got %+v", got)
	}

	if got := run(t, d, ctx, "EXPIRE", "k", "100"); got.Integer != 1 {
		t.Fatalf("expected EXPIRE=1, got %+v", got)
	}
	if got := run(t, d, ctx, "TTL", "k"); got.Integer <= 0 || got.Integer > 100 {
		t.Fatalf("expected TTL in (0,100], got %+v", got)
	}
	if got := run(t, d, ctx, "PERSIST", "k"); got.Integer != 1 {
		t.Fatalf("expected PERSIST=1, got %+v", got)
	}
	if got := run(t, d, ctx, "TTL", "k"); got.Integer != -1 {
		t.Fatalf("expected -1 after PERSIST, got %+v", got)
	}
}

func TestDBSizeAndFlushAll(t *testing.T) {
	d, ctx := NewDispatcher(), newTestContext()
	run(t, d, ctx, "SET", "a", "1")
	run(t, d, ctx, "SET", "b", "2")
	if got := run(t, d, ctx, "DBSIZE"); got.Integer != 2 {
		t.Fatalf("got %+v", got)
	}
	run(t, d, ctx, "FLUSHALL")
	if got := run(t, d, ctx, "DBSIZE"); got.Integer != 0 {
		t.Fatalf("got %+v", got)
	}
}

func TestInfoContainsExpectedFields(t *testing.T) {
	d, ctx := NewDispatcher(), newTestContext()
	run(t, d, ctx, "SET", "a", "1")
	run(t, d, ctx, "GET", "a")
	run(t, d, ctx, "GET", "missing")

	got := run(t, d, ctx, "INFO")
	if got.Type != resp.RespBulkString {
		t.Fatalf("expected bulk string, got %+v", got)
	}
	body := string(got.Bulk)
	for _, want := range []string{"uptime_seconds:", "total_commands_processed:", "keyspace_hits:1",
		"keyspace_misses:1", "db0:keys=1", "used_memory_bytes:"} {
		if !strings.Contains(body, want) {
			t.Fatalf("expected INFO output to contain %q, got:\n%s", want, body)
		}
	}
}

func TestSaveWithNoSnapshotPathConfigured(t *testing.T) {
	d, ctx := NewDispatcher(), newTestContext()
	ctx.Config.SnapshotPath = ""
	got := run(t, d, ctx, "SAVE")
	if got.Type != resp.RespError {
		t.Fatalf("expected error when persistence is disabled, got %+v", got)
	}
}

func TestSaveWritesRealSnapshot(t *testing.T) {
	d, ctx := NewDispatcher(), newTestContext()
	dir := t.TempDir()
	ctx.Config.SnapshotPath = dir + "/snap.bin"

	run(t, d, ctx, "SET", "k", "v")
	got := run(t, d, ctx, "SAVE")
	if got.Type != resp.RespSimpleString || got.String != "OK" {
		t.Fatalf("SAVE failed: %+v", got)
	}
}

func TestUnknownCommand(t *testing.T) {
	d, ctx := NewDispatcher(), newTestContext()
	got := run(t, d, ctx, "FROBNICATE")
	if got.Type != resp.RespError {
		t.Fatalf("expected error, got %+v", got)
	}
}

func TestWrongArgCounts(t *testing.T) {
	d, ctx := NewDispatcher(), newTestContext()
	cases := [][]string{
		{"GET"}, {"GET", "a", "b"},
		{"SET"}, {"SET", "onlykey"},
		{"DEL"}, {"EXISTS"},
		{"EXPIRE", "k"}, {"PERSIST"}, {"TTL"},
		{"INCR"}, {"DECR"},
		{"TYPE"},
	}
	for _, args := range cases {
		got := d.Dispatch(args, ctx)
		if got.Type != resp.RespError {
			t.Fatalf("expected error for args %v, got %+v", args, got)
		}
	}
}
