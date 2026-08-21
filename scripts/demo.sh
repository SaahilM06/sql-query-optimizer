#!/usr/bin/env bash
# A scripted walkthrough of the whole system, meant to be run start-to-finish
# (or screen-recorded) to show every major piece working for real: single-
# node planning, the plan cache, real execution, EXPLAIN ANALYZE, ANALYZE,
# a distributed cluster with a live fault-tolerance demonstration, and the
# adaptive bandit. Every command here is real -- nothing in this script is
# printed or faked, it drives the actual binaries.
#
# Run from the repo root after building: cmake -S . -B build && cmake --build build -j 8
set -euo pipefail
cd "$(dirname "$0")/.."

pause() { echo; read -r -p "-- press enter to continue -- " _; echo; }
say() { echo; echo "=== $* ==="; echo; }

say "1. Single-node CLI: parse, cost-based plan, real execution"
echo 'SELECT c.name FROM customers c WHERE c.country = '"'"'US'"'"'
EXPLAIN SELECT c.name FROM customers c JOIN orders o ON c.id = o.customer_id
EXIT' | ./build/sql_optimizer_cli
pause

say "2. Real statistics from real data, and how much it improves the estimate"
echo 'SELECT c.name FROM customers c WHERE c.country = '"'"'US'"'"'
ANALYZE
SELECT c.name FROM customers c WHERE c.country = '"'"'US'"'"'
EXIT' | ./build/sql_optimizer_cli
pause

say "3. Starting the plan cache (Go, RESP protocol) and the distributed cluster (3 real worker processes)"
( cd cache && go run ./cmd/server -addr :6380 -snapshot-path "" > /tmp/demo_cache.log 2>&1 & )
sleep 1
./build/sql_optimizer_worker 0 3 7001 > /tmp/demo_worker_0.log 2>&1 &
./build/sql_optimizer_worker 1 3 7002 > /tmp/demo_worker_1.log 2>&1 &
./build/sql_optimizer_worker 2 3 7003 > /tmp/demo_worker_2.log 2>&1 &
sleep 2
echo "cache + 3 workers running"
pause

say "4. Distributed execution: broadcast vs. shuffle, chosen by a bandit that's learned from real latency"
echo 'SELECT c.name FROM customers c JOIN orders o ON c.id = o.customer_id WHERE c.country = '"'"'US'"'"'
SHOW BANDIT
EXIT' | ./build/sql_optimizer_coordinator
pause

say "5. Fault tolerance: kill a worker mid-cluster, watch the coordinator recover and still return the correct result"
WORKER1_PID=$(pgrep -f "sql_optimizer_worker 1 3" | head -1)
echo "killing worker 1 (pid $WORKER1_PID)..."
kill -9 "$WORKER1_PID"
sleep 1
echo 'SELECT o.customer_id, SUM(o.total), COUNT(*) FROM orders o GROUP BY o.customer_id
EXIT' | ./build/sql_optimizer_coordinator
pause

say "6. Cleaning up"
pkill -f "sql_optimizer_worker" 2>/dev/null || true
pkill -f "cmd/server" 2>/dev/null || true
echo "done"
