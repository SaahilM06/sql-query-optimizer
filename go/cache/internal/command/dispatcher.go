// Package command implements the RESP command surface: parsing already
// happened (internal/resp), storage already exists (internal/storage) --
// this package's only job is validating arguments, calling the right
// storage method, and building the RESP reply. It knows nothing about
// sockets.
package command

import (
	"strings"
	"time"

	"sqlopt/cache/internal/config"
	"sqlopt/cache/internal/metrics"
	"sqlopt/cache/internal/resp"
	"sqlopt/cache/internal/storage"
)

// Context bundles everything a command handler needs. Passed by pointer so
// every handler shares the same store/metrics/config/start-time.
type Context struct {
	Store     *storage.ShardedStore
	Metrics   *metrics.Metrics
	Config    *config.Config
	StartTime time.Time
}

// Handler implements one command. args excludes the command name itself
// (e.g. for "SET k v", args is ["k", "v"]).
type Handler func(args []string, ctx *Context) resp.RespValue

// Dispatcher routes a parsed command name to its Handler.
type Dispatcher struct {
	handlers map[string]Handler
}

func NewDispatcher() *Dispatcher {
	d := &Dispatcher{handlers: make(map[string]Handler)}
	registerStringCommands(d)
	registerExpiryCommands(d)
	registerAdminCommands(d)
	return d
}

func (d *Dispatcher) register(name string, h Handler) {
	d.handlers[name] = h
}

// Dispatch runs the command named by args[0] against ctx. args[0] itself is
// stripped before the handler sees it.
func (d *Dispatcher) Dispatch(args []string, ctx *Context) resp.RespValue {
	if len(args) == 0 {
		return resp.Error("ERR empty command")
	}
	ctx.Metrics.CommandsProcessed.Add(1)

	name := strings.ToUpper(args[0])
	handler, ok := d.handlers[name]
	if !ok {
		return resp.Error("ERR unknown command '" + args[0] + "'")
	}
	return handler(args[1:], ctx)
}

func wrongArgs(cmd string) resp.RespValue {
	return resp.Error("ERR wrong number of arguments for '" + cmd + "' command")
}
