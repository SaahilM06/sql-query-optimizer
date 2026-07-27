// Package server implements the TCP front end: accept connections, spawn a
// goroutine per connection, and support graceful shutdown. It has no idea
// what a command means -- that's internal/command's job.
package server

import (
	"context"
	"errors"
	"net"
	"sync"
	"time"

	"sqlopt/cache/internal/command"
	"sqlopt/cache/internal/metrics"
)

type Server struct {
	dispatcher *command.Dispatcher
	cmdCtx     *command.Context
	metrics    *metrics.Metrics

	listener net.Listener

	mu    sync.Mutex
	conns map[net.Conn]struct{}
	wg    sync.WaitGroup
}

func New(dispatcher *command.Dispatcher, cmdCtx *command.Context, m *metrics.Metrics) *Server {
	return &Server{
		dispatcher: dispatcher,
		cmdCtx:     cmdCtx,
		metrics:    m,
		conns:      make(map[net.Conn]struct{}),
	}
}

// Serve binds addr and accepts connections, spawning one goroutine per
// connection, until ctx is canceled (at which point the listener is closed
// and Serve returns nil) or a non-transient Accept error occurs.
func (s *Server) Serve(ctx context.Context, addr string) error {
	ln, err := net.Listen("tcp", addr)
	if err != nil {
		return err
	}
	s.listener = ln

	stopWatcher := make(chan struct{})
	defer close(stopWatcher)
	go func() {
		select {
		case <-ctx.Done():
			ln.Close()
		case <-stopWatcher:
		}
	}()

	for {
		conn, err := ln.Accept()
		if err != nil {
			if errors.Is(err, net.ErrClosed) {
				return nil
			}
			continue // transient accept error -- keep serving
		}

		s.trackConn(conn)
		s.metrics.Connections.Add(1)
		s.metrics.TotalConnections.Add(1)

		s.wg.Add(1)
		go func() {
			defer s.wg.Done()
			defer s.untrackConn(conn)
			defer s.metrics.Connections.Add(^uint64(0)) // atomic decrement (no signed Add on Uint64)
			handleConnection(conn, s.dispatcher, s.cmdCtx)
		}()
	}
}

func (s *Server) trackConn(c net.Conn) {
	s.mu.Lock()
	s.conns[c] = struct{}{}
	s.mu.Unlock()
}

func (s *Server) untrackConn(c net.Conn) {
	s.mu.Lock()
	delete(s.conns, c)
	s.mu.Unlock()
}

// Shutdown stops accepting new connections and waits up to gracePeriod for
// in-flight connections to finish on their own (i.e. their client sends no
// more commands and disconnects). Any still open after gracePeriod are
// force-closed so their blocked Read() calls unblock and the handler
// goroutines can exit.
func (s *Server) Shutdown(gracePeriod time.Duration) {
	if s.listener != nil {
		s.listener.Close()
	}

	done := make(chan struct{})
	go func() {
		s.wg.Wait()
		close(done)
	}()

	select {
	case <-done:
		return
	case <-time.After(gracePeriod):
	}

	s.mu.Lock()
	for c := range s.conns {
		c.Close()
	}
	s.mu.Unlock()

	<-done
}
