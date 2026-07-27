# Plan Cache

A small Redis-compatible in-memory key/value store written in Go, built as a
standalone service. It speaks RESP (the Redis wire protocol) over TCP, so
any Redis client library — or `redis-cli` itself — can talk to it. It's
being built to eventually cache serialized query execution plans from a
separate C++ SQL optimizer, but has no dependency on that project and works
as a general-purpose small cache on its own.

Zero external dependencies — standard library only.

## Architecture

```
                 ┌─────────────────────────────┐
                 │       CLI / Test Client      │
                 │   redis-cli style commands   │
                 └──────────────┬───────────────┘
                                │ TCP
                                ▼
                 ┌─────────────────────────────┐
                 │         TCP Server           │
                 │  accepts multiple clients    │
                 │  one goroutine per client    │
                 └──────────────┬───────────────┘
                                │
                                ▼
                 ┌─────────────────────────────┐
                 │         RESP Parser          │
                 │   bytes → command arguments  │
                 │   handles partial messages   │
                 └──────────────┬───────────────┘
                                │
                                ▼
                 ┌─────────────────────────────┐
                 │      Command Dispatcher      │
                 │  validates and routes cmds   │
                 └──────────────┬───────────────┘
                                │
              ┌─────────────────┼─────────────────┐
              ▼                 ▼                 ▼
      ┌──────────────┐  ┌──────────────┐  ┌──────────────┐
      │ Sharded K/V  │  │  Expiration  │  │ LRU Eviction │
      │    Store     │  │   (lazy +    │  │  (per shard) │
      │  (64 shards) │  │   active)    │  │              │
      └──────┬───────┘  └──────┬───────┘  └──────┬───────┘
             │                 │                 │
             └─────────────────┴─────────────────┘
                               │
                               ▼
                 ┌─────────────────────────────┐
                 │       Persistence Layer      │
                 │     binary snapshot          │
                 │   save/load, atomic rename   │
                 └─────────────────────────────┘

                 ┌─────────────────────────────┐
                 │      Metrics (atomics)       │
                 │  hits, misses, evictions,    │
                 │  expirations, connections    │
                 └─────────────────────────────┘
```

### Package layout

```
cache/
├── cmd/
│   ├── server/     entry point: flags, signal handling, wiring everything together
│   ├── client/     redis-cli-style interactive/one-shot client
│   └── benchmark/  load generator + throughput/latency report
├── internal/
│   ├── server/     TCP accept loop, one goroutine per connection, graceful shutdown
│   ├── resp/       RESP protocol: RespValue, ParseRESP, WriteRESP
│   ├── command/    command dispatcher + handlers (strings/expiry/admin)
│   ├── storage/    sharded store, per-shard LRU + TTL, eviction
│   ├── persistence/  binary snapshot save/load
│   ├── metrics/    atomic counters exposed via INFO
│   └── config/     server configuration
└── tests/
    └── integration/  real server, real TCP, end-to-end behavior
```

Connection handling knows nothing about cache logic — it reads a RESP
frame, hands it to the dispatcher, writes back whatever RESP value comes
out. The dispatcher knows nothing about sockets. Storage knows nothing
about RESP or commands, just keys/values/TTLs/eviction. This is what makes
each layer independently testable (see `internal/*/*_test.go`).

## Supported commands

| Command | Notes |
|---|---|
| `PING [message]` | |
| `ECHO message` | |
| `SET key value [EX seconds \| PX ms] [NX \| XX]` | NX = only if absent, XX = only if present |
| `GET key` | |
| `DEL key [key ...]` | |
| `EXISTS key [key ...]` | |
| `EXPIRE key seconds` | |
| `PERSIST key` | removes an existing TTL |
| `TTL key` | `-2` = missing, `-1` = no expiry |
| `INCR key` / `DECR key` | errors on a non-integer value |
| `MSET key value [key value ...]` | |
| `MGET key [key ...]` | |
| `TYPE key` | always `string` or `none` — no list/set/hash types |
| `DBSIZE` | |
| `FLUSHALL` | |
| `INFO` | uptime, hit rate, evictions, expirations, memory, connections |
| `SAVE` | writes a snapshot immediately |

`KEYS` is deliberately not implemented — a full keyspace scan is expensive
and not something the eventual optimizer-cache integration should be able
to rely on (see [Cache invalidation](#cache-invalidation-design-for-later)
below).

## Concurrency model

The store is split into 64 shards, each with its own mutex, its own
key→entry map, and its own LRU list. A key's shard is chosen by
`fnv32a(key) % numShards`. This means two unrelated keys almost never
contend on the same lock, which is what lets throughput scale with
concurrent client count (see [Benchmarks](#benchmarks) below) instead of
serializing on one global mutex.

The TCP server spawns one goroutine per connection — no manually managed
worker pool. Each connection has its own buffered reader/writer over the
raw socket. A `bufio.Reader` blocking on a short read is what makes partial
TCP packets, multiple pipelined commands in one read, and long-lived
persistent connections all work correctly without any extra framing logic:
`ParseRESP` just keeps reading until it has a complete value, however many
underlying `Read()` calls that takes.

## Expiration strategy

Two mechanisms, matching Redis:

- **Lazy**: every `GET`/`EXISTS`/`TTL` checks the entry's expiry before
  returning it, and deletes it on the spot if expired.
- **Active**: a background goroutine (`ShardedStore.RunExpirationWorker`)
  sweeps every shard every 250ms (configurable), removing anything expired
  even if it's never touched again.

The active sweep is a full scan of each shard's entries every tick, not
sampling. That's simpler and was fine at this cache's scale in testing; a
production Redis-scale deployment would sample a bounded number of keys per
tick instead to cap worst-case pause time on a very large keyspace. Worth
knowing if this ever needs to scale to millions of keys per shard.

## Eviction policy

Each shard tracks recency with a `container/list`-based LRU: the front is
most-recently-used, the back is the next eviction candidate. `GET` and
`SET` both move a key to the front. Eviction is driven by an approximate
per-entry byte size (`len(key) + len(value) + 64` bytes of struct/bookkeeping
overhead), not just an entry count — a memory budget is a much more useful
knob than "max N keys" when values vary in size. The `-max-memory-mb` flag
sets the total budget, split evenly across shards.

The overhead constant doesn't have to be exact, only consistent, so
eviction behaves predictably.

## Persistence

`SAVE` (and an optional periodic auto-save, `-snapshot-interval`) writes a
custom binary snapshot:

```
magic bytes    "GOCACHE1"   (8 bytes)
format version uint32
entry count    uint64
for each entry:
  key length   uint32
  key bytes
  value length uint32
  value bytes
  has_expiry   byte (0/1)
  expires_at   int64 (Unix nanoseconds)
checksum       uint32 (CRC32-IEEE over everything above)
```

Save is crash-safe: the whole payload is built in memory, written to a
temp file in the same directory, `fsync`'d, then atomically renamed over
the destination. A crash mid-write can never leave a corrupted file at the
real path — the old snapshot (if any) stays intact until the rename
succeeds. Load verifies the checksum and rejects anything corrupted, and
drops any entry that had already expired by the time it's being restored.

On startup, the server loads `-snapshot-path` if it exists (a missing file
is not an error — nothing to restore yet). On `SIGINT`/`SIGTERM`, it saves
a final snapshot before exiting.

## Graceful shutdown

`SIGINT`/`SIGTERM` triggers:

1. Stop accepting new connections (close the listener).
2. Cancel the background expiration/snapshot workers.
3. Wait up to `-shutdown-grace` (default 5s) for in-flight connections to
   finish on their own.
4. Force-close any still open after that, so their blocked `Read()` calls
   unblock and the handler goroutines can exit.
5. Save a final snapshot.
6. Exit.

## Running it

```bash
# Server
go run ./cmd/server -addr :6380 -max-memory-mb 128 -snapshot-path cache.snapshot

# Interactive CLI
go run ./cmd/client -addr localhost:6380
127.0.0.1:6380> SET plan:123 abc
OK
127.0.0.1:6380> GET plan:123
"abc"

# One-shot
go run ./cmd/client -addr localhost:6380 GET plan:123

# Benchmark
go run ./cmd/benchmark -address localhost:6380 -clients 32 -requests 100000 -ratio-get 0.8
```

## Testing

```bash
go test ./...                        # unit + integration (78 tests)
go test ./... -race                  # concurrency correctness
go test -fuzz=FuzzParseRESP ./internal/resp/   # protocol parser fuzzing
```

- **Unit tests** (colocated with each package): RESP encode/decode, SET/GET,
  NX/XX, expiration, LRU ordering, eviction under a memory budget, INCR/DECR,
  snapshot save/load round trip, checksum corruption rejection, metrics.
- **Integration tests** (`tests/integration/`): a real `server.Server` bound
  to a real TCP port — multiple commands on one connection, concurrent
  clients, pipelined commands, a command frame deliberately split across two
  separate `Write()` calls, expiration under a live server, a full
  save → restart → load → verify cycle, and graceful shutdown actually
  refusing new connections afterward.
- **Fuzzing**: `FuzzParseRESP` throws arbitrary bytes at the parser. This
  caught a real bug during development — see below.
- **Concurrency**: a stress test hammers overlapping keys across all 64
  shards from 32 goroutines simultaneously; the whole suite runs clean under
  `-race`.

### A bug the fuzzer actually found

Early on, `go test -fuzz=FuzzParseRESP` found that a frame like
`*777777772\r\n` — a RESP array header declaring ~777 million elements —
made the parser try to `make([]RespValue, 777777772)` *before* validating
anything else about the input. That's an easy memory-exhaustion attack
against an otherwise-correct parser: any TCP client can open a connection
and hang or OOM the server with a single line of bytes. The same issue
existed for oversized bulk-string length prefixes, and independently for
length-prefixed fields when loading a corrupted snapshot file.

Fixed by rejecting any length prefix above a fixed bound (matching Redis's
own `proto-max-bulk-len` default of 512MB for bulk strings; 1M elements for
arrays) *before* allocating anything. Regression tests for both cases live
in `internal/resp/resp_test.go`, and the fuzzer now runs clean for
millions of executions.

## Benchmarks

Measured with `cmd/benchmark` against a locally running server (8-core
machine), 80% GET / 20% SET workload, 10,000-key keyspace, 100-byte values:

| Clients | Throughput (ops/sec) | p50 | p95 | p99 |
|--:|--:|--:|--:|--:|
| 1   | 37,274  | 0.03 ms | 0.04 ms | 0.06 ms |
| 8   | 101,259 | 0.07 ms | 0.11 ms | 0.24 ms |
| 32  | 125,484 | 0.22 ms | 0.44 ms | 0.79 ms |
| 64  | 118,982 | 0.55 ms | 0.82 ms | 1.36 ms |
| 128 | 113,145 | 1.16 ms | 1.47 ms | 2.44 ms |

Throughput peaks around 32–64 concurrent clients on this 8-core machine and
flattens/dips slightly beyond that — expected, since each client here does
a synchronous request/response per operation (no pipelining), so past the
point where all cores are busy, additional clients just queue rather than
add throughput.

Eviction under memory pressure, verified directly (not just unit-tested):
16 clients, 50,000-key keyspace, 200-byte values, a 2MB memory budget —
**35,408 keys evicted** over 100,000 requests, throughput unaffected
(119,151 ops/sec), confirming eviction doesn't introduce a bottleneck.

### Results are stored, not just printed

Every `cmd/benchmark` run appends a JSON record to `benchmarks/results.jsonl`
by default (`-output`, one JSON object per line so concurrent/repeated runs
can't corrupt earlier entries). The table above is generated from real
entries in that file, not hand-typed — `git log -p -- go/cache/benchmarks/results.jsonl`
gives an actual history of measured performance over time, e.g. to check
whether a change regressed throughput or latency. Tag a run with `-label`
(a git commit, a scenario name) to make that history easier to slice later.

Reproduce with:

```bash
go run ./cmd/server -addr :6380 -snapshot-path ""
go run ./cmd/benchmark -address localhost:6380 -clients 32 -requests 200000 -ratio-get 0.8 -label "$(git rev-parse --short HEAD)"
```

## Known limitations

- **String values only** — no lists, sets, hashes, or sorted sets. This is
  intentional: the cache exists to store serialized blobs (eventually query
  plans), not to be a general Redis replacement.
- **RESP2 only** — no RESP3, no `HELLO`, no client-side caching support.
- **No auth, no TLS, no ACLs.** Meant to run on a trusted internal network.
- **No replication, no clustering.** Single process, single node.
- **`KEYS` is not implemented** — see cache invalidation design below for
  why a full scan shouldn't be relied on anyway.
- **Active expiration is a full per-shard scan**, not sampling — fine at
  this scale, would need revisiting for a much larger keyspace.
- **No write-ahead log** — only point-in-time snapshots. A crash between
  saves loses writes since the last snapshot (same trade-off Redis's RDB
  persistence makes without AOF).

## Cache invalidation design (for later)

Not implemented yet, but the key format is already designed around it so
integration doesn't require a redesign later. A cached query plan's key
would look like:

```
plan:<query-hash>:<schema-version>:<stats-version>
```

e.g. `plan:8e48fa12:7:24`. When the schema or statistics change, the
version segment changes — the optimizer simply stops requesting the old
key, and TTL eventually reclaims it. No wildcard scan-and-delete needed,
which matters because `KEYS`-style scans are exactly what this cache
avoids exposing as a reliable primitive.

## Optimizer integration (not yet built)

This cache is deliberately standalone right now — no dependency on the C++
optimizer, and vice versa. The planned integration:

```
C++ optimizer
    │ normalize SQL, hash it, build a versioned key (see above)
    ▼
RESP GET over TCP to this cache
    ├── hit:  deserialize the cached physical plan, use it
    └── miss: run the optimizer, serialize the result, RESP SET with a TTL
```

Serialization format for cross-language plan values is still an open
choice — Protocol Buffers or MessagePack are the leading candidates over
JSON (for size/speed) or Go's `gob` (which is Go-specific and unsuitable
for a value a C++ process needs to read).
