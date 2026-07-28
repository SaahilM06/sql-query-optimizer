package command

import (
	"fmt"
	"strconv"
	"strings"
	"time"

	"sqlopt/cache/internal/resp"
	"sqlopt/cache/internal/storage"
)

func registerStringCommands(d *Dispatcher) {
	d.register("PING", handlePing)
	d.register("ECHO", handleEcho)
	d.register("SET", handleSet)
	d.register("GET", handleGet)
	d.register("DEL", handleDel)
	d.register("EXISTS", handleExists)
	d.register("INCR", handleIncr)
	d.register("DECR", handleDecr)
	d.register("MSET", handleMSet)
	d.register("MGET", handleMGet)
	d.register("TYPE", handleType)
}

func handlePing(args []string, _ *Context) resp.RespValue {
	switch len(args) {
	case 0:
		return resp.SimpleString("PONG")
	case 1:
		return resp.BulkStr(args[0])
	default:
		return wrongArgs("PING")
	}
}

func handleEcho(args []string, _ *Context) resp.RespValue {
	if len(args) != 1 {
		return wrongArgs("ECHO")
	}
	return resp.BulkStr(args[0])
}

// parseSetOptions parses SET's trailing modifiers, in any order:
// [EX seconds | PX milliseconds] [NX | XX]
func parseSetOptions(args []string) (storage.SetOptions, error) {
	var opts storage.SetOptions
	i := 0
	for i < len(args) {
		switch strings.ToUpper(args[i]) {
		case "EX":
			if i+1 >= len(args) {
				return opts, fmt.Errorf("syntax error")
			}
			secs, err := strconv.Atoi(args[i+1])
			if err != nil || secs < 0 {
				return opts, fmt.Errorf("invalid expire time in 'SET' command")
			}
			opts.HasTTL = true
			opts.TTL = time.Duration(secs) * time.Second
			i += 2
		case "PX":
			if i+1 >= len(args) {
				return opts, fmt.Errorf("syntax error")
			}
			ms, err := strconv.Atoi(args[i+1])
			if err != nil || ms < 0 {
				return opts, fmt.Errorf("invalid expire time in 'SET' command")
			}
			opts.HasTTL = true
			opts.TTL = time.Duration(ms) * time.Millisecond
			i += 2
		case "NX":
			opts.NX = true
			i++
		case "XX":
			opts.XX = true
			i++
		default:
			return opts, fmt.Errorf("syntax error")
		}
	}
	if opts.NX && opts.XX {
		return opts, fmt.Errorf("syntax error")
	}
	return opts, nil
}

func handleSet(args []string, ctx *Context) resp.RespValue {
	if len(args) < 2 {
		return wrongArgs("SET")
	}
	key, value := args[0], args[1]

	opts, err := parseSetOptions(args[2:])
	if err != nil {
		return resp.Error("ERR " + err.Error())
	}

	applied := ctx.Store.Set(key, []byte(value), opts)
	if !applied {
		return resp.Null()
	}
	return resp.SimpleString("OK")
}

func handleGet(args []string, ctx *Context) resp.RespValue {
	if len(args) != 1 {
		return wrongArgs("GET")
	}
	v, ok := ctx.Store.Get(args[0])
	if !ok {
		return resp.Null()
	}
	return resp.Bulk(v)
}

func handleDel(args []string, ctx *Context) resp.RespValue {
	if len(args) == 0 {
		return wrongArgs("DEL")
	}
	return resp.Integer(int64(ctx.Store.Delete(args...)))
}

func handleExists(args []string, ctx *Context) resp.RespValue {
	if len(args) == 0 {
		return wrongArgs("EXISTS")
	}
	var count int64
	for _, k := range args {
		if ctx.Store.Exists(k) {
			count++
		}
	}
	return resp.Integer(count)
}

func handleIncr(args []string, ctx *Context) resp.RespValue {
	if len(args) != 1 {
		return wrongArgs("INCR")
	}
	v, err := ctx.Store.IncrBy(args[0], 1)
	if err != nil {
		return resp.Error("ERR " + err.Error())
	}
	return resp.Integer(v)
}

func handleDecr(args []string, ctx *Context) resp.RespValue {
	if len(args) != 1 {
		return wrongArgs("DECR")
	}
	v, err := ctx.Store.IncrBy(args[0], -1)
	if err != nil {
		return resp.Error("ERR " + err.Error())
	}
	return resp.Integer(v)
}

func handleMSet(args []string, ctx *Context) resp.RespValue {
	if len(args) == 0 || len(args)%2 != 0 {
		return resp.Error("ERR wrong number of arguments for 'MSET' command")
	}
	for i := 0; i < len(args); i += 2 {
		ctx.Store.Set(args[i], []byte(args[i+1]), storage.SetOptions{})
	}
	return resp.SimpleString("OK")
}

func handleMGet(args []string, ctx *Context) resp.RespValue {
	if len(args) == 0 {
		return wrongArgs("MGET")
	}
	items := make([]resp.RespValue, len(args))
	for i, k := range args {
		if v, ok := ctx.Store.Get(k); ok {
			items[i] = resp.Bulk(v)
		} else {
			items[i] = resp.Null()
		}
	}
	return resp.Array(items)
}

func handleType(args []string, ctx *Context) resp.RespValue {
	if len(args) != 1 {
		return wrongArgs("TYPE")
	}
	if ctx.Store.Exists(args[0]) {
		return resp.SimpleString("string")
	}
	return resp.SimpleString("none")
}
