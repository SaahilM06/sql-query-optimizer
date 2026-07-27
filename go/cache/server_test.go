package cache

import (
	"bufio"
	"net"
	"testing"
	"time"
)

// startTestServer boots a Server on an OS-assigned port and returns a
// connected client reader/writer plus a cleanup func.
func startTestServer(t *testing.T) (conn net.Conn, r *bufio.Reader, cleanup func()) {
	t.Helper()

	c := New(100)
	srv := NewServer(c)

	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("listen: %v", err)
	}
	srv.listener = ln

	go func() {
		for {
			conn, err := ln.Accept()
			if err != nil {
				return
			}
			go srv.handleConn(conn)
		}
	}()

	conn, err = net.Dial("tcp", ln.Addr().String())
	if err != nil {
		t.Fatalf("dial: %v", err)
	}

	cleanup = func() {
		conn.Close()
		ln.Close()
		c.Close()
	}
	return conn, bufio.NewReader(conn), cleanup
}

func sendCommand(t *testing.T, conn net.Conn, r *bufio.Reader, args ...string) Value {
	t.Helper()
	items := make([]Value, len(args))
	for i, a := range args {
		items[i] = BulkString([]byte(a))
	}
	if _, err := conn.Write(Array(items).Encode()); err != nil {
		t.Fatalf("write: %v", err)
	}
	resp, err := ReadValue(r)
	if err != nil {
		t.Fatalf("read response: %v", err)
	}
	return resp
}

func TestServerPing(t *testing.T) {
	conn, r, cleanup := startTestServer(t)
	defer cleanup()

	resp := sendCommand(t, conn, r, "PING")
	if resp.Type != TypeSimpleString || resp.Str != "PONG" {
		t.Fatalf("unexpected PING response: %+v", resp)
	}
}

func TestServerSetGet(t *testing.T) {
	conn, r, cleanup := startTestServer(t)
	defer cleanup()

	resp := sendCommand(t, conn, r, "SET", "plan:1", "hash-join(orders,customers)")
	if resp.Type != TypeSimpleString || resp.Str != "OK" {
		t.Fatalf("unexpected SET response: %+v", resp)
	}

	resp = sendCommand(t, conn, r, "GET", "plan:1")
	if resp.Type != TypeBulkString || string(resp.Bulk) != "hash-join(orders,customers)" {
		t.Fatalf("unexpected GET response: %+v", resp)
	}
}

func TestServerGetMissingReturnsNullBulk(t *testing.T) {
	conn, r, cleanup := startTestServer(t)
	defer cleanup()

	resp := sendCommand(t, conn, r, "GET", "nope")
	if resp.Type != TypeBulkString || !resp.Null {
		t.Fatalf("expected null bulk string, got %+v", resp)
	}
}

func TestServerSetWithExpiry(t *testing.T) {
	conn, r, cleanup := startTestServer(t)
	defer cleanup()

	sendCommand(t, conn, r, "SET", "plan:ttl", "v", "EX", "1")

	ttlResp := sendCommand(t, conn, r, "TTL", "plan:ttl")
	if ttlResp.Type != TypeInteger || ttlResp.Int <= 0 || ttlResp.Int > 1 {
		t.Fatalf("expected TTL in (0, 1], got %+v", ttlResp)
	}

	notFoundTTL := sendCommand(t, conn, r, "TTL", "no-such-key")
	if notFoundTTL.Int != -2 {
		t.Fatalf("expected -2 for missing key TTL, got %+v", notFoundTTL)
	}

	sendCommand(t, conn, r, "SET", "plan:persist", "v")
	persistTTL := sendCommand(t, conn, r, "TTL", "plan:persist")
	if persistTTL.Int != -1 {
		t.Fatalf("expected -1 for key with no expiration, got %+v", persistTTL)
	}
}

func TestServerDelAndExists(t *testing.T) {
	conn, r, cleanup := startTestServer(t)
	defer cleanup()

	sendCommand(t, conn, r, "SET", "a", "1")
	sendCommand(t, conn, r, "SET", "b", "2")

	existsResp := sendCommand(t, conn, r, "EXISTS", "a", "b", "missing")
	if existsResp.Int != 2 {
		t.Fatalf("expected EXISTS count 2, got %+v", existsResp)
	}

	delResp := sendCommand(t, conn, r, "DEL", "a", "missing")
	if delResp.Int != 1 {
		t.Fatalf("expected DEL count 1, got %+v", delResp)
	}

	getResp := sendCommand(t, conn, r, "GET", "a")
	if getResp.Type != TypeBulkString || !getResp.Null {
		t.Fatalf("expected 'a' to be gone after DEL, got %+v", getResp)
	}
}

func TestServerUnknownCommand(t *testing.T) {
	conn, r, cleanup := startTestServer(t)
	defer cleanup()

	resp := sendCommand(t, conn, r, "FROBNICATE")
	if resp.Type != TypeError {
		t.Fatalf("expected error response, got %+v", resp)
	}
}

func TestServerConcurrentClients(t *testing.T) {
	conn, r, cleanup := startTestServer(t)
	defer cleanup()

	// Populate via the first connection, then confirm a second connection
	// dialed at the same address sees the same shared cache state.
	sendCommand(t, conn, r, "SET", "shared", "value")

	addr := conn.LocalAddr()
	_ = addr // first conn already established; open a second directly
	conn2, err := net.DialTimeout("tcp", conn.RemoteAddr().String(), time.Second)
	if err != nil {
		t.Fatalf("second dial: %v", err)
	}
	defer conn2.Close()
	r2 := bufio.NewReader(conn2)

	resp := sendCommand(t, conn2, r2, "GET", "shared")
	if resp.Type != TypeBulkString || string(resp.Bulk) != "value" {
		t.Fatalf("expected second client to see shared state, got %+v", resp)
	}
}
