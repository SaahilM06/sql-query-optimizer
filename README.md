# SQL Query Optimizer

A cost-based, distributed, adaptive SQL query engine built from scratch in
C++ (the optimizer, execution engine, and distributed layer) and Go (the
plan cache) — zero external dependencies anywhere in the stack. Every
number and behavior claimed in this README was actually run and observed,
not simulated; see [`benchmarks/results.jsonl`](benchmarks/results.jsonl)
and [`ROADMAP.md`](ROADMAP.md) for the receipts.

```sql
SELECT c.name FROM customers c
JOIN orders o ON c.id = o.customer_id
WHERE c.country = 'US';
```

Type that into the CLI and the engine parses it, chooses a join order and
physical algorithm by cost (not by the order you wrote the joins in), checks
a Redis-compatible plan cache written in Go over a hand-rolled RESP client,
executes it for real against real data, and — if you point it at a live
worker cluster instead — decides whether to broadcast or shuffle the join
using a bandit that's learned from real observed latency, tolerates a
worker dying mid-query by recomputing that worker's share itself, and logs
everything it did.

## What's actually here

- **A real cost-based optimizer**: hand-written lexer/parser, a 3-pass
  logical rewrite optimizer (predicate/projection pushdown, join
  reordering), a Selinger-style dynamic-programming join-order search that
  chooses join order *and* physical algorithm together, statistics-driven
  cardinality estimation with histograms.
- **A real execution engine**: a pull-iterator model (`open`/`next`/`close`)
  running against CSV-backed in-memory tables, not a plan-printer that
  stops at "here's what I'd do."
- **A real plan cache**: a Redis-compatible server in Go (RESP protocol,
  sharded LRU+TTL, crash-safe snapshots) with the C++ side talking to it
  over a hand-rolled RESP client — full details in
  [`cache/README.md`](cache/README.md).
- **A real distributed layer**: separate worker *processes* (not
  threads-pretending-to-be-nodes) communicating over hand-rolled HTTP,
  choosing between broadcast and shuffle joins, recovering from a worker
  dying mid-query by recomputing its share on the coordinator.
- **A real adaptive layer**: an epsilon-greedy bandit that learns broadcast-
  vs-shuffle from observed latency, and an online-regression cost
  calibrator that learns to convert the optimizer's abstract cost units
  into predicted milliseconds from real `EXPLAIN ANALYZE` runs.
- **A real benchmark suite** with committed, reproducible results.

All 19 items on the original build roadmap are done — see
[`ROADMAP.md`](ROADMAP.md) for the full vision, what's built, and what's
explicitly out of scope and why.

## Architecture

```
                    CLI  /  Web Explorer  /  Coordinator
                            │
   SQL text → Lexer → Parser → AST
                            │
        Logical Optimizer (predicate/projection pushdown, join reorder)
                            │
        Join Search (join graph → Selinger DP → join order + algorithm)
                            │
        Physical Planner (SeqScan/IndexScan, NestedLoop/Hash/IndexNL join,
                           HashAggregate, Sort, Limit, Project)
                            │
        Cost Model (cardinality estimation, CPU/IO/memory cost)
                            │
                    ┌───────┴────────┐
                    ▼                ▼
              Go Plan Cache     Execution Engine
              (RESP/TCP)        (pull-iterator, real rows)
                                     │
                    ┌────────────────┴────────────────┐
                    ▼                                  ▼
            Single-node execution          Distributed coordinator
                                                        │
                                     ┌──────────────────┼──────────────────┐
                                     ▼                  ▼                  ▼
                                 Worker 0           Worker 1           Worker 2
                            (static partition) (static partition) (static partition)
                                     │                  │                  │
                                     └────── Broadcast / Shuffle ──────────┘
                                          (bandit-chosen, fault-tolerant)
                                                        │
                                                        ▼
                                          Adaptive layer: bandit + cost
                                          calibration, learning from real
                                          EXPLAIN ANALYZE / query telemetry
```

## Quick start

```bash
# Build everything
cmake -S . -B build && cmake --build build -j 8

# Run the C++ test suite (112 tests) and the Go cache's own suite
./build/sql_optimizer_tests
cd cache && go test ./... && cd ..
```

### Interactive CLI (single-node)

```bash
./build/sql_optimizer_cli
sql> SELECT c.name FROM customers c WHERE c.country = 'US'
sql> EXPLAIN SELECT c.name FROM customers c JOIN orders o ON c.id = o.customer_id
sql> EXPLAIN ANALYZE SELECT c.name FROM customers c JOIN orders o ON c.id = o.customer_id
sql> ANALYZE
sql> SHOW STATS
sql> SHOW CALIBRATION
```

### Plan cache (optional — the CLI works standalone without it)

```bash
cd cache && go run ./cmd/server -addr :6380 -snapshot-path ""
```

### Visual plan explorer

```bash
./build/sql_optimizer_web
# open http://127.0.0.1:8080 -- type SQL, see the plan tree with
# estimated-vs-actual highlighted where they diverge sharply
```

### Distributed cluster

```bash
./build/sql_optimizer_worker 0 3 7001 &
./build/sql_optimizer_worker 1 3 7002 &
./build/sql_optimizer_worker 2 3 7003 &
./build/sql_optimizer_coordinator
sql> SELECT c.name FROM customers c JOIN orders o ON c.id = o.customer_id WHERE c.country = 'US'
sql> SHOW BANDIT
```

Kill one of the worker processes mid-session and run a query again — the
coordinator detects it, recomputes that worker's partition itself, and
returns the identical result. This is real, not a fallback message; see
"Fault tolerance" below for the actual numbers.

### Benchmark suite

```bash
SQLOPT_BENCHMARK_ITERATIONS=8 ./build/sql_optimizer_benchmark
# runs the single-node suite always; runs the distributed suite too if
# workers (above) are reachable. Appends to benchmarks/results.jsonl.
```

### Docker (one command)

```bash
docker compose up --build
# cache + 3 workers + web explorer at http://localhost:8080

# the coordinator is a REPL, not a long-running service, so it's a
# separate profile-gated invocation once the above is up:
docker compose run --rm coordinator
```

Every container binds via `SQLOPT_BIND_HOST=0.0.0.0` and talks to the others
by Docker Compose service name (`cache:6380`, `worker0:7001`, ...) — see
`docker-compose.yml`. Verified live, including killing a worker container
mid-cluster and confirming the coordinator (in its own container) still
returns the correct result.

## Real numbers from `benchmarks/results.jsonl`

Measured on an 8-core laptop, 3 workers, 8 iterations/query, real cache
server, real worker processes:

| Experiment | Finding |
|---|---|
| Cardinality q-error | Ranges from ~5× to ~8000× across query shapes, reflecting the gap between the hand-authored `stats/*.json` fixtures (deliberately production-scale) and the real, small `data/` dataset — exactly the gap `ANALYZE` exists to close (see below). |
| Network latency (0ms → 50ms simulated per request) | A 3-round-trip single-table query: ~13ms → ~195ms. A broadcast join: ~731ms → ~1205ms. Consistent with the round-trip-count model the exchange strategies were designed around. |
| `ANALYZE` on real data | Regenerating statistics from `data/` instead of `stats/*.json` took a `country = 'US'` filter's row estimate from 495 (wrong by ~5×) to 38 (much closer to the observed 96) — a real, measured accuracy improvement, not a claim. |
| Fixed-rule vs. adaptive (broadcast/shuffle) | In an 8-iteration/query run, did **not** show a strong adaptive advantage — epsilon-greedy hasn't converged yet at that sample size. A longer, single-query manual session (12+ iterations) elsewhere in this project's history did show clear convergence to the faster strategy. Reported here honestly rather than cherry-picked. |
| Fault tolerance | Killing 2 of 3 workers mid-cluster still produced results identical to the healthy-cluster baseline, recomputing both missing partitions on the coordinator. |

## Known limitations (stated, not hidden)

- **No real index acceleration in execution yet.** `IndexScan`/
  `IndexNestedLoopJoin` execute as their non-indexed equivalents —
  correct output, not yet the speed the optimizer's cost model assumes.
  This shows up directly in the benchmark numbers above.
- **Distribution only covers the outermost join.** A 3+ table join falls
  back to single-node execution with a logged reason. Full nested-exchange
  placement needs the optimizer to reason about per-subtree partitioning
  (a fuller "physical property tracking" than what's built).
- **The bandit is epsilon-greedy, not LinUCB/Thompson Sampling** — the
  simplest version on purpose, per the original design's own ordering.
- **No skew or fully-automated straggler experiments** — the knobs exist
  (`SQLOPT_WORKER_DELAY_MS`) but the benchmark harness doesn't orchestrate
  restarting a worker mid-run with them set.
- **This build is Unix-only** (POSIX sockets throughout) and meant for a
  trusted local network — no auth, no TLS, on either the C++ or Go side.

## Repository layout

```
parser/ logical/ physical/ statistics/ optimizer/   C++ optimizer core
storage/ execution/                                 real single-node execution
integration/                                         C++ <-> Go cache wiring
distributed/                                         workers, coordinator, exchange
adaptive/                                             bandit + cost calibration
web/                                                  visual plan explorer (HTTP + JS)
metrics/                                              per-query telemetry
cmd/{demo,cli,web,worker,coordinator,benchmark}/      entry points
cache/                                                the Go plan cache (own README)
data/ stats/                                          sample dataset + hand-authored stats
tests/ benchmarks/                                    C++ test suite + real benchmark results
```

See [`ROADMAP.md`](ROADMAP.md) for the full build history, design
rationale for every major decision, and exactly what's left.
