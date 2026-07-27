// Integration tests: a real server.Server bound to a real (OS-assigned)
// TCP port, exercised over actual net.Conn connections -- not the command
// dispatcher called directly in-process, which is what internal/command's
// unit tests already cover.
package integration

import (
	"bufio"
	"bytes"
	"context"
	"fmt"
	"net"
	"path/filepath"
	"sync"
	"testing"
	"time"

	"sqlopt/cache/internal/command"
	"sqlopt/cache/internal/config"
	"sqlopt/cache/internal/metrics"
	"sqlopt/cache/internal/persistence"
	"sqlopt/cache/internal/resp"
	"sqlopt/cache/internal/server"
	"sqlopt/cache/internal/storage"
)

// encodeToBytes renders a RespValue to its raw wire bytes -- useful when a
// test needs to control exactly how those bytes are written to the
// connection (e.g. split across multiple Write calls).
func encodeToBytes(v resp.RespValue) []byte {
	var buf bytes.Buffer
	w := bufio.NewWriter(&buf)
	_ = resp.WriteRESP(w, v)
	_ = w.Flush()
	return buf.Bytes()
}

type testServer struct {
	addr    string
	store   *storage.ShardedStore
	metrics *metrics.Metrics
	cfg     *config.Config
	srv     *server.Server
	cancel  context.CancelFunc
	done    chan struct{}
}

func startTestServer(t *testing.T, cfg config.Config) *testServer {
	t.Helper()

	m := metrics.New()
	store := storage.New(cfg.NumShards, cfg.MaxMemoryBytes, m)
	cmdCtx := &command.Context{Store: store, Metrics: m, Config: &cfg, StartTime: m.StartTime}
	dispatcher := command.NewDispatcher()
	srv := server.New(dispatcher, cmdCtx, m)

	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("listen: %v", err)
	}
	addr := ln.Addr().String()
	ln.Close() // Serve() re-binds; we only needed a free port

	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan struct{})
	go func() {
		_ = srv.Serve(ctx, addr)
		close(done)
	}()

	waitForListener(t, addr)

	return &testServer{addr: addr, store: store, metrics: m, cfg: &cfg, srv: srv, cancel: cancel, done: done}
}

func waitForListener(t *testing.T, addr string) {
	t.Helper()
	deadline := time.Now().Add(2 * time.Second)
	for time.Now().Before(deadline) {
		conn, err := net.Dial("tcp", addr)
		if err == nil {
			conn.Close()
			return
		}
		time.Sleep(5 * time.Millisecond)
	}
	t.Fatalf("server never started listening on %s", addr)
}

func (ts *testServer) stop(t *testing.T) {
	t.Helper()
	ts.cancel()
	select {
	case <-ts.done:
	case <-time.After(2 * time.Second):
		t.Fatal("server did not shut down in time")
	}
}

// dialErr/sendCommandErr return errors instead of calling t.Fatalf, so they
// are safe to use from goroutines other than the test's own -- the testing
// package requires FailNow (which t.Fatalf calls) to only ever be invoked
// from the goroutine running the test function itself.
func dialErr(addr string) (net.Conn, *bufio.Reader, error) {
	conn, err := net.Dial("tcp", addr)
	if err != nil {
		return nil, nil, err
	}
	return conn, bufio.NewReader(conn), nil
}

func sendCommandErr(conn net.Conn, r *bufio.Reader, args ...string) (resp.RespValue, error) {
	w := bufio.NewWriter(conn)
	if err := resp.WriteRESP(w, resp.EncodeCommand(args...)); err != nil {
		return resp.RespValue{}, fmt.Errorf("write: %w", err)
	}
	if err := w.Flush(); err != nil {
		return resp.RespValue{}, fmt.Errorf("flush: %w", err)
	}
	v, err := resp.ParseRESP(r)
	if err != nil {
		return resp.RespValue{}, fmt.Errorf("read reply: %w", err)
	}
	return v, nil
}

// dial/sendCommand are the t.Fatalf-on-error convenience wrappers for use
// directly in the test's own goroutine.
func dial(t *testing.T, addr string) (net.Conn, *bufio.Reader) {
	t.Helper()
	conn, r, err := dialErr(addr)
	if err != nil {
		t.Fatalf("dial: %v", err)
	}
	return conn, r
}

func sendCommand(t *testing.T, conn net.Conn, r *bufio.Reader, args ...string) resp.RespValue {
	t.Helper()
	v, err := sendCommandErr(conn, r, args...)
	if err != nil {
		t.Fatalf("%v", err)
	}
	return v
}

// ── Multiple commands, one connection ────────────────────────────────────────

func TestMultipleCommandsOnOneConnection(t *testing.T) {
	ts := startTestServer(t, config.Default())
	defer ts.stop(t)

	conn, r := dial(t, ts.addr)
	defer conn.Close()

	if got := sendCommand(t, conn, r, "SET", "a", "1"); got.String != "OK" {
		t.Fatalf("SET: %+v", got)
	}
	if got := sendCommand(t, conn, r, "GET", "a"); string(got.Bulk) != "1" {
		t.Fatalf("GET: %+v", got)
	}
	if got := sendCommand(t, conn, r, "DEL", "a"); got.Integer != 1 {
		t.Fatalf("DEL: %+v", got)
	}
	if got := sendCommand(t, conn, r, "GET", "a"); !got.IsNull() {
		t.Fatalf("GET after DEL: %+v", got)
	}
}

// ── Multiple concurrent clients share server state ───────────────────────────

func TestMultipleConcurrentClients(t *testing.T) {
	ts := startTestServer(t, config.Default())
	defer ts.stop(t)

	conn1, r1 := dial(t, ts.addr)
	defer conn1.Close()
	sendCommand(t, conn1, r1, "SET", "shared", "value")

	conn2, r2 := dial(t, ts.addr)
	defer conn2.Close()
	got := sendCommand(t, conn2, r2, "GET", "shared")
	if string(got.Bulk) != "value" {
		t.Fatalf("expected second client to see first client's write, got %+v", got)
	}

	const n = 16
	var wg sync.WaitGroup
	errs := make(chan error, n)
	for i := 0; i < n; i++ {
		wg.Add(1)
		go func(id int) {
			defer wg.Done()
			c, cr, err := dialErr(ts.addr)
			if err != nil {
				errs <- fmt.Errorf("client %d: dial: %w", id, err)
				return
			}
			defer c.Close()
			got, err := sendCommandErr(c, cr, "PING")
			if err != nil {
				errs <- fmt.Errorf("client %d: %w", id, err)
				return
			}
			if got.String != "PONG" {
				errs <- fmt.Errorf("client %d: PING failed, got %+v", id, got)
			}
		}(i)
	}
	wg.Wait()
	close(errs)
	for err := range errs {
		t.Error(err)
	}
}

// ── Pipelining: multiple commands written before reading any reply ──────────

func TestPipelinedCommands(t *testing.T) {
	ts := startTestServer(t, config.Default())
	defer ts.stop(t)

	conn, r := dial(t, ts.addr)
	defer conn.Close()

	w := bufio.NewWriter(conn)
	resp.WriteRESP(w, resp.EncodeCommand("SET", "a", "1"))
	resp.WriteRESP(w, resp.EncodeCommand("SET", "b", "2"))
	resp.WriteRESP(w, resp.EncodeCommand("GET", "a"))
	resp.WriteRESP(w, resp.EncodeCommand("GET", "b"))
	if err := w.Flush(); err != nil {
		t.Fatalf("flush: %v", err)
	}

	for _, want := range []string{"OK", "OK"} {
		got, err := resp.ParseRESP(r)
		if err != nil {
			t.Fatalf("read reply: %v", err)
		}
		if got.String != want {
			t.Fatalf("got %+v, want SimpleString(%q)", got, want)
		}
	}
	for _, want := range []string{"1", "2"} {
		got, err := resp.ParseRESP(r)
		if err != nil {
			t.Fatalf("read reply: %v", err)
		}
		if string(got.Bulk) != want {
			t.Fatalf("got %+v, want Bulk(%q)", got, want)
		}
	}
}

// ── A command frame split across multiple separate Writes ───────────────────

func TestCommandSplitAcrossMultipleWrites(t *testing.T) {
	ts := startTestServer(t, config.Default())
	defer ts.stop(t)

	conn, r := dial(t, ts.addr)
	defer conn.Close()

	full := encodeToBytes(resp.EncodeCommand("SET", "fragmented", "value"))
	mid := len(full) / 2

	if _, err := conn.Write(full[:mid]); err != nil {
		t.Fatalf("first partial write: %v", err)
	}
	time.Sleep(20 * time.Millisecond) // give the server a chance to block on the incomplete frame
	if _, err := conn.Write(full[mid:]); err != nil {
		t.Fatalf("second partial write: %v", err)
	}

	got, err := resp.ParseRESP(r)
	if err != nil {
		t.Fatalf("read reply: %v", err)
	}
	if got.String != "OK" {
		t.Fatalf("got %+v", got)
	}
}

// ── Expiration under a real running server ───────────────────────────────────

func TestExpirationUnderRealServer(t *testing.T) {
	ts := startTestServer(t, config.Default())
	defer ts.stop(t)

	conn, r := dial(t, ts.addr)
	defer conn.Close()

	sendCommand(t, conn, r, "SET", "short", "v", "PX", "30")
	if got := sendCommand(t, conn, r, "GET", "short"); got.IsNull() {
		t.Fatal("expected key present immediately after SET")
	}
	time.Sleep(60 * time.Millisecond)
	if got := sendCommand(t, conn, r, "GET", "short"); !got.IsNull() {
		t.Fatalf("expected key expired, got %+v", got)
	}
}

// ── Restart and restore from snapshot ─────────────────────────────────────────

func TestRestartAndRestoreFromSnapshot(t *testing.T) {
	path := filepath.Join(t.TempDir(), "snap.bin")
	cfg := config.Default()
	cfg.SnapshotPath = path

	first := startTestServer(t, cfg)
	conn, r := dial(t, first.addr)
	sendCommand(t, conn, r, "SET", "persisted", "value")
	conn.Close()

	if err := persistence.SaveSnapshot(first.store, path); err != nil {
		t.Fatalf("SaveSnapshot: %v", err)
	}
	first.stop(t)

	second := startTestServer(t, cfg)
	defer second.stop(t)
	if err := persistence.LoadSnapshot(second.store, path); err != nil {
		t.Fatalf("LoadSnapshot: %v", err)
	}

	conn2, r2 := dial(t, second.addr)
	defer conn2.Close()
	got := sendCommand(t, conn2, r2, "GET", "persisted")
	if string(got.Bulk) != "value" {
		t.Fatalf("expected restored key after restart, got %+v", got)
	}
}

// ── Graceful shutdown ─────────────────────────────────────────────────────────

func TestGracefulShutdownStopsAcceptingNewConnections(t *testing.T) {
	ts := startTestServer(t, config.Default())

	conn, r := dial(t, ts.addr)
	sendCommand(t, conn, r, "PING")

	ts.stop(t) // cancels ctx -> Serve() closes the listener and returns

	if _, err := net.DialTimeout("tcp", ts.addr, 200*time.Millisecond); err == nil {
		t.Fatal("expected connection to a shut-down server to fail")
	}
	conn.Close()
}
