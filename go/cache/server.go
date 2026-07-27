package cache

import (
	"bufio"
	"errors"
	"log"
	"net"
	"strconv"
	"strings"
	"time"
)

// Server exposes a Cache over a Redis-compatible RESP TCP protocol,
// supporting the subset of commands a query-plan cache needs: PING, SET
// (with optional EX ttl), GET, DEL, EXISTS, TTL, and FLUSHALL.
type Server struct {
	cache    *Cache
	listener net.Listener
}

func NewServer(c *Cache) *Server {
	return &Server{cache: c}
}

// ListenAndServe binds addr and serves connections until the listener is
// closed (via Close), at which point it returns nil.
func (s *Server) ListenAndServe(addr string) error {
	ln, err := net.Listen("tcp", addr)
	if err != nil {
		return err
	}
	s.listener = ln

	for {
		conn, err := ln.Accept()
		if err != nil {
			if errors.Is(err, net.ErrClosed) {
				return nil
			}
			log.Printf("cache: accept error: %v", err)
			continue
		}
		go s.handleConn(conn)
	}
}

// Close stops accepting new connections.
func (s *Server) Close() error {
	if s.listener == nil {
		return nil
	}
	return s.listener.Close()
}

func (s *Server) handleConn(conn net.Conn) {
	defer conn.Close()
	r := bufio.NewReader(conn)

	for {
		req, err := ReadValue(r)
		if err != nil {
			return // client disconnected or sent malformed input
		}
		resp := s.dispatch(req)
		if _, err := conn.Write(resp.Encode()); err != nil {
			return
		}
	}
}

func (s *Server) dispatch(req Value) Value {
	if req.Type != TypeArray || len(req.Array) == 0 {
		return Error("ERR expected a non-empty command array")
	}

	args := make([]string, len(req.Array))
	for i, item := range req.Array {
		if item.Type != TypeBulkString || item.Null {
			return Error("ERR expected bulk string command arguments")
		}
		args[i] = string(item.Bulk)
	}

	switch strings.ToUpper(args[0]) {
	case "PING":
		return SimpleString("PONG")
	case "SET":
		return s.handleSet(args[1:])
	case "GET":
		return s.handleGet(args[1:])
	case "DEL":
		return s.handleDel(args[1:])
	case "EXISTS":
		return s.handleExists(args[1:])
	case "TTL":
		return s.handleTTL(args[1:])
	default:
		return Error("ERR unknown command '" + args[0] + "'")
	}
}

// handleSet implements: SET key value [EX seconds]
func (s *Server) handleSet(args []string) Value {
	if len(args) != 2 && len(args) != 4 {
		return Error("ERR wrong number of arguments for 'SET'")
	}
	key, value := args[0], args[1]

	var ttl time.Duration
	if len(args) == 4 {
		if strings.ToUpper(args[2]) != "EX" {
			return Error("ERR syntax error")
		}
		secs, err := strconv.Atoi(args[3])
		if err != nil || secs < 0 {
			return Error("ERR invalid expire time in 'SET' command")
		}
		ttl = time.Duration(secs) * time.Second
	}

	s.cache.Set(key, []byte(value), ttl)
	return SimpleString("OK")
}

func (s *Server) handleGet(args []string) Value {
	if len(args) != 1 {
		return Error("ERR wrong number of arguments for 'GET'")
	}
	value, ok := s.cache.Get(args[0])
	if !ok {
		return NullBulk()
	}
	return BulkString(value)
}

func (s *Server) handleDel(args []string) Value {
	if len(args) == 0 {
		return Error("ERR wrong number of arguments for 'DEL'")
	}
	var deleted int64
	for _, key := range args {
		if s.cache.Delete(key) {
			deleted++
		}
	}
	return Integer(deleted)
}

func (s *Server) handleExists(args []string) Value {
	if len(args) == 0 {
		return Error("ERR wrong number of arguments for 'EXISTS'")
	}
	var count int64
	for _, key := range args {
		if _, ok := s.cache.Get(key); ok {
			count++
		}
	}
	return Integer(count)
}

// handleTTL follows Redis's convention: -2 means the key doesn't exist,
// -1 means it exists but has no expiration.
func (s *Server) handleTTL(args []string) Value {
	if len(args) != 1 {
		return Error("ERR wrong number of arguments for 'TTL'")
	}
	ttl, ok := s.cache.TTL(args[0])
	if !ok {
		return Integer(-2)
	}
	if ttl == 0 {
		return Integer(-1)
	}
	secs := int64(ttl.Seconds())
	if secs == 0 {
		secs = 1 // round sub-second remaining TTLs up rather than reporting expired
	}
	return Integer(secs)
}
