# Roadmap: Adaptive Distributed SQL Engine

## Vision

Turn this from "I implemented a query optimizer" into a full end-to-end systems
project: a distributed SQL execution engine with a cost-based and adaptive query
optimizer, a standalone Go plan cache, real execution, runtime telemetry, an
interactive plan visualizer, and reproducible benchmarks showing how optimization
decisions affect latency.

Everything already built stays useful — nothing here replaces the existing C++
optimizer or Go cache. Every phase below layers a new system around what already
works.

Target demo: type a query, watch it get parsed, planned (N candidate plans
considered, one selected with its estimated cost), checked against the cache,
executed across a small worker cluster with visible per-stage latency, and see
the optimizer's estimate vs. reality — then watch the adaptive layer change its
mind about the right plan as conditions (network delay, data skew, a slow
worker) change over a run of queries.

## End-state architecture

```
CLIENT / UI  (SQL editor | plan viewer | metrics dashboard | benchmarks)
        │ HTTP / CLI
        ▼
COORDINATOR
  SQL Frontend: Lexer → Parser → AST
        │
  Logical Optimizer: predicate pushdown, projection pruning, rewrites
        │
  Join Search: join graph, Selinger DP, memoization
        │
  Physical Planner: seq/index scan, hash/NL join, agg, sort, exchange
        │
  Cost Model: cardinality, CPU, I/O, memory, network
        │
  Plan Choice ──┬──────────────────┐
        │       ▼                  ▼
        │   Go Plan Cache    Execution Scheduler
        │   (RESP/TCP)       (stage/DAG builder)
        ▼
WORKER CLUSTER (N workers: scan, filter, join, agg, shuffle/broadcast/exchange)
        │
        ▼
Runtime Telemetry (rows, bytes, latency, CPU, memory, stages)
        │
        ▼
Adaptive Optimizer (online regression / contextual bandits)
        │
        └──► feeds back into future cost estimates
```

The point is that the arrows form a closed loop — the optimizer doesn't just
decide, execution tells it whether it was right.

## Three tiers (each one is a legitimate stopping point)

- **Tier 1 — strong project (done):** C++ SQL optimizer, statistics, cardinality
  estimation, cost-aware join enumeration, physical plans, Go Redis-compatible
  plan cache, the two wired together, benchmarks for the cache itself.
- **Tier 2 — exceptional end-to-end project:** add a real executor, EXPLAIN
  ANALYZE, runtime metrics, a web plan visualizer, real query execution
  benchmarks. Now it's a database system, not just an optimizer.
- **Tier 3 — research/systems showcase:** add distributed execution,
  shuffle/broadcast decisions, physical property tracking, an adaptive cost
  model, bandit exploration, P99 experiments, fault handling. Now it's "an
  adaptive distributed SQL engine built largely from scratch."

## Build sequence

Status legend: ✅ done · ⬅️ in progress/next · ⬜ not started

- ✅ Cost-aware join search (`optimizer/join_graph`, `access_path_generator`,
  `join_enumerator` — Selinger-style DP, join order + algorithm chosen together)
- ✅ Go cache integration (`cache/` + `integration/` — RESP client, versioned
  cache keys, get-or-compute planning, verified live)
- ✅ EXPLAIN + optimizer CLI (`cmd/cli/` — interactive REPL: bare SQL executes
  and prints rows, `EXPLAIN <sql>`, `EXPLAIN ANALYZE <sql>`, `SHOW STATS`,
  `SHOW CACHE`; `SHOW WORKERS` stubbed until a worker cluster exists)
- ✅ Real single-node execution (`storage/` + `execution/` — pull-iterator
  model, CSV-backed tables in `data/`, one executor per `PhysicalPlan::Kind`;
  IndexScan/IndexNestedLoopJoin execute as their non-indexed equivalents for
  now, correctness-equivalent but not yet accelerated by a real index)
- ✅ EXPLAIN ANALYZE (`execution/query_runner.cpp` — walks the PhysicalPlan
  and executor trees in lockstep, prints estimated vs. actual rows/time at
  every node; cumulative per-node timing falls out of the executor's
  open()/next() wrappers for free, no manual bookkeeping)
- ✅ Real statistics generation (`statistics/analyzer.cpp` — `ANALYZE` in the
  CLI scans `data/` into a real StatisticsCatalog: NDV, null fraction,
  min/max, a 10-bucket equi-width histogram; folds a fresh epoch into the
  cache key's stats_version so post-ANALYZE plans don't collide with plans
  cached under the old JSON-fixture statistics)
- ✅ Metrics infrastructure (`metrics/` — per-query ID, parse/cache-lookup/
  plan/cache-store/execution timings, estimated-vs-actual rows, logged to
  `metrics/query_log.jsonl` and printed after every CLI query; candidate-plan
  counts and distributed-stage timings deferred until join search exposes a
  counter and a worker cluster exists, respectively)
- ✅ Visual plan explorer, minimal version (`web/` — hand-rolled HTTP/1.1
  server, `GET /api/schema` + `POST /api/query`, `web/frontend/index.html`
  vanilla-JS tree renderer with estimated-vs-actual divergence highlighting;
  verified in a real browser). Not yet done: rejected-alternative-plans
  view (needs the join enumerator to expose candidates it didn't pick, not
  just the winner), live updates, dedicated frontend tooling
- ✅ Distributed coordinator/workers (`distributed/`, `cmd/worker/`,
  `cmd/coordinator/` — real separate OS processes over HTTP, not simulated;
  workers statically partition every table by its own primary key,
  `id % num_workers`)
- ✅ Exchange operators — Broadcast and Shuffle (`distributed/coordinator.cpp`
  — broadcast gathers a small join side from all workers and replicates it;
  shuffle gathers both sides to the coordinator and rehashes by the *join
  key*, real data movement that fixes up whatever the static partitioning
  didn't already align)
- ✅ Broadcast/Shuffle/Local hash join variants — broadcast vs. shuffle
  chosen by a fixed row-count threshold (not yet integrated into the
  cost-based DP search — see "physical property tracking" below); local
  join at each worker reuses the existing HashJoinExec/NestedLoopJoinExec
  unchanged via an ExternalRows-injected input
- ✅ Distributed aggregation (partial per-worker aggregate → coordinator
  merge; AVG split into SUM+COUNT before shipping to workers since
  per-worker averages can't be correctly re-averaged)
- ✅ Physical property tracking, narrow version (`distributed/properties.hpp`
  — tracks one property, shuffle-join co-location by the join key, for one
  consumer: skipping the AVG-split-and-recombine dance and the
  cross-worker merge when a GROUP BY matches the shuffle key, since each
  worker's partial aggregate is then already final. Not yet done: the
  fuller version that would let a 3+ table join distribute at more than
  one exchange point — that still falls back to single-node, logged with a
  clear reason, same as before)
- ✅ Adaptive runtime model (`adaptive/calibration.hpp` — online least-squares
  regression learning a per-operator-kind cost-unit → ms correction from
  real EXPLAIN ANALYZE runs; `SHOW CALIBRATION` in the CLI. Deliberately
  never feeds back into the join-order/algorithm search itself — an
  additive, inspectable layer, not a change to planning)
- ✅ Bandit-based plan selection (`adaptive/bandit.hpp` — epsilon-greedy,
  contextualized by which relations a join involves, per ROADMAP's own
  "epsilon-greedy first" — replaces the fixed broadcast-vs-shuffle rule;
  `SHOW BANDIT` in the coordinator. UCB/Thompson/LinUCB not built — this is
  deliberately the simplest version, not the strongest one)
- ✅ Fault injection/recovery (`distributed/coordinator.cpp`'s
  `call_worker_or_recover` — a dead worker's contribution is recomputed by
  the coordinator itself, filtering its own already-loaded
  `local_full_database` down to exactly that worker's partition via the
  same `partition_of()` math every worker uses at startup, so the result is
  provably identical, not a guess. Verified with real `kill -9` against a
  live worker process — including a double failure, 1 of 3 workers left
  standing — producing results identical to the healthy-cluster baseline
  every time)
- ✅ Full benchmarking study (`cmd/benchmark/` — 6-query suite, real p50/p95/p99
  latency, real cardinality q-error, cache cold-vs-warm, single-node-vs-
  distributed, a real fixed-rule-vs-adaptive comparison via
  `SQLOPT_FORCE_STRATEGY`, and a real network-condition experiment;
  results committed in `benchmarks/results.jsonl`, not fabricated. No
  "naive planner" mode — see the file's own top comment for why; that
  comparison already exists as the
  `join_order_differs_from_sql_syntax_order_when_cheaper` test. Skew and
  a fully-automated straggler run are not built — `SQLOPT_WORKER_DELAY_MS`
  exists for a manual straggler comparison but the harness doesn't
  orchestrate restarting a worker with it set)
- ✅ Presentation polish (`README.md` — pitch, architecture diagram, quick
  start, real benchmark numbers, known limitations; `scripts/demo.sh` — a
  scripted live walkthrough including a real `kill -9` fault-tolerance
  demo; `Dockerfile` + `cache/Dockerfile` + `docker-compose.yml` — one-
  command cluster launch (`docker compose up --build`, then
  `docker compose run --rm coordinator`), verified live: built both
  images, brought up cache + 3 workers + web, confirmed the web API
  reaches the cache over the compose network, ran a real distributed join
  and a real fault-tolerance test — `docker kill -s SIGKILL` on a worker
  container, coordinator still returned the correct full result — from a
  separate coordinator container talking to the workers by service name.
  Along the way, fixed two real bugs Docker surfaced that local dev
  never hit: `cmd/web`/`cmd/worker` hardcoded their HTTP server to bind
  `127.0.0.1` only (unreachable from other containers or via port
  mapping — now configurable via `SQLOPT_BIND_HOST`, default unchanged),
  and `util::JsonValue` held itself via `unordered_map`, which requires
  a complete value type and compiled only by accident under macOS's
  libc++ — GCC/libstdc++ (the Docker image's toolchain) correctly
  rejected it; fixed by switching to a small vector-backed ordered map
  (`JsonObject`), the same incomplete-type-friendly pattern `array_val`
  already used one line above it.

## Subsystem notes (condensed from the original design discussion)

- **SQL frontend**: enough of SQL to create interesting optimization problems,
  not full ANSI SQL — SELECT/FROM/JOIN/WHERE/GROUP BY/HAVING/ORDER BY/LIMIT,
  AND/OR/NOT, comparisons, arithmetic, COUNT/SUM/AVG/MIN/MAX. Already covers
  this.
- **Logical rewrites**: predicate pushdown, projection pruning, constant
  folding, filter simplification, limit pushdown, join predicate extraction.
  Predicate/projection pushdown and join reordering already exist; constant
  folding and filter simplification are not yet implemented.
- **Statistics catalog**: row/page counts, NDV, null fraction, min/max,
  histograms per column, already loaded from JSON. `ANALYZE` (scan real data
  to generate these instead of hand-authoring them) is a later phase.
- **Physical operators**: SeqScan, IndexScan, Filter, NestedLoopJoin,
  IndexNestedLoopJoin, HashJoin (MergeJoin is a stretch goal), HashAggregate
  (SortAggregate is a stretch goal), Sort, TopK, Limit, Project — all but
  MergeJoin/SortAggregate/TopK already exist.
- **Execution engine**: pull-iterator model first (`open`/`next`/`close`),
  vectorized batches (`next_batch()` of ~1024 rows) as a later performance
  pass. Needs real backing storage — CSV first, a compact binary/columnar
  format later (unlocks SIMD filtering, compression, real predicate pushdown
  to storage).
- **Distributed execution**: N worker processes (local first, Dockerized
  later), hash-partitioned tables, an Exchange operator family (Broadcast,
  Shuffle), a stage/DAG scheduler with dependency tracking
  (`Stage 1, Stage 2 → Stage 3 → Stage 4`).
- **Physical properties**: track partitioning/ordering through the plan so the
  optimizer can recognize when a shuffle or sort is unnecessary because the
  input already has the needed property.
- **Adaptive optimizer**: record estimated-vs-actual per operator, correct
  cost-model coefficients with online regression, then layer a contextual
  bandit (context = row counts, selectivity, network load, worker count,
  memory pressure; actions = broadcast/shuffle/local, or any other
  cost-tied physical choice; reward = negative latency) for decisions where
  the static cost model's confidence is low or conditions are visibly
  drifting.
- **Observability**: per-query ID, planning-phase timings, cache lookup time,
  execution stage timings, per-operator estimated vs. actual rows/cost/time,
  distributed bytes shuffled/broadcast, worker/stage skew.
- **Visual plan explorer**: SQL editor → plan tree with per-node estimated vs.
  actual, clickable operator detail, and — a strong demo feature — the
  rejected alternative plans and their costs shown alongside the chosen one.
- **Benchmark suite**: TPC-H-inspired or real TPC-H at small scale (1/5/10GB),
  query categories from single-table filters through 5-table joins and
  aggregations, comparing naive/rule-based/cost-based/adaptive planning modes.
  Report q-error (`max(actual/estimated, estimated/actual)`) for cardinality
  accuracy, plus p50/p95/p99 latency for planning, execution, and cache ops.
- **Performance engineering**: profile with perf/FlameGraph/heap profiling once
  correctness is settled; candidate wins are arena allocation for plan nodes,
  flatter data structures, string interning, batch execution, memory pooling
  — each reported as a measured before/after, not a claim.
- **Fault handling**: detect a dropped worker connection mid-stage, mark the
  stage failed, retry the lost partition on another worker; demoable via
  deliberate fault injection.

## Working agreement for this build

This is being built in checkpointed batches of ~2-3 phases at a time: implement,
verify (tests + a real run, not just "it compiles"), report back with what
changed and what was decided along the way, then wait for the go-ahead before
continuing. Each checkpoint's work gets committed on completion unless told
otherwise. Architectural decisions the original design left open (iterator vs.
vectorized execution, CSV vs. binary storage, bandit algorithm choice, etc.) get
the sensible default noted in this doc unless a phase's checkpoint report flags
a real fork in the road worth stopping for.
