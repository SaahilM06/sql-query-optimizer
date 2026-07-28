package command

import (
	"strconv"
	"time"

	"sqlopt/cache/internal/resp"
)

func registerExpiryCommands(d *Dispatcher) {
	d.register("EXPIRE", handleExpire)
	d.register("PERSIST", handlePersist)
	d.register("TTL", handleTTL)
}

func handleExpire(args []string, ctx *Context) resp.RespValue {
	if len(args) != 2 {
		return wrongArgs("EXPIRE")
	}
	secs, err := strconv.Atoi(args[1])
	if err != nil {
		return resp.Error("ERR value is not an integer or out of range")
	}
	if !ctx.Store.Expire(args[0], time.Duration(secs)*time.Second) {
		return resp.Integer(0)
	}
	return resp.Integer(1)
}

func handlePersist(args []string, ctx *Context) resp.RespValue {
	if len(args) != 1 {
		return wrongArgs("PERSIST")
	}
	if ctx.Store.Persist(args[0]) {
		return resp.Integer(1)
	}
	return resp.Integer(0)
}

// handleTTL follows Redis's convention: -2 means the key doesn't exist, -1
// means it exists but has no expiration set.
func handleTTL(args []string, ctx *Context) resp.RespValue {
	if len(args) != 1 {
		return wrongArgs("TTL")
	}
	ttl, ok := ctx.Store.TTL(args[0])
	if !ok {
		return resp.Integer(-2)
	}
	if ttl == 0 {
		return resp.Integer(-1)
	}
	secs := int64(ttl.Seconds())
	if secs == 0 {
		secs = 1 // round a sub-second remaining TTL up rather than reporting expired
	}
	return resp.Integer(secs)
}
