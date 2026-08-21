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
- ⬅️ **Real statistics generation** — an `ANALYZE` command that scans the
  data/ tables now that they exist, instead of relying only on hand-authored
  JSON fixtures
- ⬜ Metrics infrastructure — per-query planning/execution telemetry, a query ID,
  candidate-plan counts
- ⬜ Visual plan explorer — HTTP API + web frontend rendering the plan tree,
  estimated vs. actual, rejected alternative plans and their costs
- ⬜ Distributed coordinator/workers — multiple worker processes, partitioned
  tables (`hash(key) % N`)
- ⬜ Exchange operators — Broadcast and Shuffle
- ⬜ Broadcast/Shuffle/Local hash join variants, cost-compared by the optimizer
- ⬜ Distributed aggregation — partial-aggregate → shuffle → final-aggregate
- ⬜ Physical property tracking — partitioning/ordering, so the optimizer can
  skip a shuffle when both sides are already co-partitioned
- ⬜ Adaptive runtime model — online regression correcting cost-model
  coefficients from actual execution telemetry
- ⬜ Bandit-based plan selection — contextual bandit (epsilon-greedy → UCB →
  LinUCB) choosing among cost-tied physical alternatives (e.g.
  broadcast vs. shuffle) under live conditions
- ⬜ Fault injection/recovery — detect a dead worker mid-stage, retry its
  partition elsewhere
- ⬜ Full benchmarking study — TPC-H-inspired dataset, query categories, naive
  vs. rule-based vs. cost-based vs. adaptive comparison, q-error metrics,
  P99 experiments under changing network/skew/straggler conditions
- ⬜ Presentation polish — Dockerized one-command cluster launch, CLI/REPL
  polish, top-level docs, a demo recording

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
