package command

import (
	"fmt"
	"strings"

	"sqlopt/cache/internal/persistence"
	"sqlopt/cache/internal/resp"
)

func registerAdminCommands(d *Dispatcher) {
	d.register("DBSIZE", handleDBSize)
	d.register("FLUSHALL", handleFlushAll)
	d.register("INFO", handleInfo)
	d.register("SAVE", handleSave)
}

func handleDBSize(args []string, ctx *Context) resp.RespValue {
	if len(args) != 0 {
		return wrongArgs("DBSIZE")
	}
	return resp.Integer(int64(ctx.Store.DBSize()))
}

func handleFlushAll(args []string, ctx *Context) resp.RespValue {
	if len(args) != 0 {
		return wrongArgs("FLUSHALL")
	}
	ctx.Store.FlushAll()
	return resp.SimpleString("OK")
}

func handleSave(args []string, ctx *Context) resp.RespValue {
	if len(args) != 0 {
		return wrongArgs("SAVE")
	}
	if ctx.Config.SnapshotPath == "" {
		return resp.Error("ERR persistence is disabled (no snapshot path configured)")
	}
	if err := persistence.SaveSnapshot(ctx.Store, ctx.Config.SnapshotPath); err != nil {
		return resp.Error("ERR SAVE failed: " + err.Error())
	}
	return resp.SimpleString("OK")
}

func handleInfo(args []string, ctx *Context) resp.RespValue {
	if len(args) != 0 {
		return wrongArgs("INFO")
	}
	snap := ctx.Metrics.Snapshot()

	var b strings.Builder
	fmt.Fprintf(&b, "# Server\r\nuptime_seconds:%.2f\r\n\r\n", snap.UptimeSeconds)
	fmt.Fprintf(&b, "# Clients\r\nconnected_clients:%d\r\ntotal_connections_received:%d\r\n\r\n",
		snap.Connections, snap.TotalConnections)
	fmt.Fprintf(&b, "# Stats\r\ntotal_commands_processed:%d\r\nkeyspace_hits:%d\r\nkeyspace_misses:%d\r\n"+
		"hit_rate:%.4f\r\nexpired_keys:%d\r\nevicted_keys:%d\r\n\r\n",
		snap.CommandsProcessed, snap.CacheHits, snap.CacheMisses, snap.HitRate, snap.ExpiredKeys, snap.EvictedKeys)
	fmt.Fprintf(&b, "# Keyspace\r\ndb0:keys=%d\r\n\r\n", ctx.Store.DBSize())
	fmt.Fprintf(&b, "# Memory\r\nused_memory_bytes:%d\r\n", snap.MemoryBytes)

	return resp.BulkStr(b.String())
}
