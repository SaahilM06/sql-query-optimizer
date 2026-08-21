// Orchestrates distributed execution across worker processes. What's
// eligible for distribution, and why:
//
// Every table in this project's schema is statically partitioned by its
// OWN primary key ("id" -- see partition.cpp), decided once when a worker
// starts up. That does NOT mean two tables are co-partitioned on whatever
// column a query happens to join them on -- orders is partitioned by
// orders.id, not orders.customer_id, so an orders<->customers join needs a
// real exchange (broadcast or shuffle) to be correct, exactly like a real
// system's static base-table partitioning would.
//
// This build handles exactly one exchange point: the query's outermost
// join, when both sides are a single base relation (broadcast the smaller
// side below a row-count threshold, else shuffle both sides by the join
// key), or a top-level GROUP BY with no join at all (distributed partial-
// aggregate + coordinator-side merge, splitting AVG into SUM+COUNT since
// per-worker averages can't be correctly averaged back together). A join
// where either side is itself a multi-relation subtree (a 3+ table query)
// would need an exchange inserted at more than one point in the tree to
// stay correct, which needs the optimizer to reason about partitioning
// per-subtree -- exactly what ROADMAP.md's "physical property tracking"
// item is for, not yet built. Those fall back to ordinary single-node
// execution rather than being distributed unsoundly.

#include "coordinator.hpp"

#include <algorithm>
#include <chrono>
#include <optional>
#include <stdexcept>
#include <unordered_map>

#include "../execution/executor_builder.hpp"
#include "../execution/query_runner.hpp"
#include "../execution/row_schema.hpp"
#include "../execution/value_ops.hpp"
#include "../integration/cached_planner.hpp"
#include "../integration/plan_serializer.hpp"
#include "../util/hash.hpp"
#include "../util/json.hpp"
#include "http_client.hpp"
#include "row_json.hpp"

namespace sql::distributed {

using namespace sql::parser;
using namespace sql::physical;
using sql::execution::RowSchema;
using sql::logical::AggregateExpr;
using sql::storage::Row;
using sql::util::JsonValue;

namespace {

constexpr size_t kBroadcastThreshold = 5000; // rows -- a simple fixed threshold, not yet part of the cost-based DP search

double ms_since(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

// ── Plan shape helpers ───────────────────────────────────────────────────────

struct UnwrapResult {
    std::vector<const PhysicalPlan*> post_ops; // Project/Sort/Limit, outermost first
    const PhysicalPlan* having = nullptr;      // Filter directly wrapping the aggregate, if any
    const PhysicalPlan* aggregate = nullptr;   // HashAggregate node, if present
    const PhysicalPlan* core = nullptr;        // join or single-relation subtree
};

UnwrapResult unwrap(const PhysicalPlan& plan) {
    UnwrapResult r;
    const PhysicalPlan* node = &plan;
    while (node->kind == PhysicalPlan::Kind::Project || node->kind == PhysicalPlan::Kind::Sort ||
           node->kind == PhysicalPlan::Kind::Limit) {
        r.post_ops.push_back(node);
        node = node->input.get();
    }
    if (node->kind == PhysicalPlan::Kind::Filter && node->input && node->input->kind == PhysicalPlan::Kind::HashAggregate) {
        r.having = node;
        r.aggregate = node->input.get();
        r.core = r.aggregate->input.get();
    } else if (node->kind == PhysicalPlan::Kind::HashAggregate) {
        r.aggregate = node;
        r.core = node->input.get();
    } else {
        r.core = node;
    }
    return r;
}

bool is_join(const PhysicalPlan& node) {
    return node.kind == PhysicalPlan::Kind::HashJoin || node.kind == PhysicalPlan::Kind::NestedLoopJoin ||
           node.kind == PhysicalPlan::Kind::IndexNestedLoopJoin;
}

const PhysicalPlan& unwrap_filter(const PhysicalPlan& node) {
    const PhysicalPlan* n = &node;
    while (n->kind == PhysicalPlan::Kind::Filter) n = n->input.get();
    return *n;
}

bool is_single_relation(const PhysicalPlan& node) {
    const PhysicalPlan& n = unwrap_filter(node);
    return n.kind == PhysicalPlan::Kind::SeqScan || n.kind == PhysicalPlan::Kind::IndexScan;
}

// A RowSchema for a (possibly Filter-wrapped) single-relation subtree,
// built from its own table_name/alias -- doesn't need live data, since the
// coordinator only needs this to resolve which side of a join condition an
// expression belongs to.
RowSchema schema_for_relation(const PhysicalPlan& node, const sql::logical::Catalog& schema_catalog) {
    const PhysicalPlan& n = unwrap_filter(node);
    RowSchema schema;
    const auto* table_schema = schema_catalog.get(n.table_name);
    if (table_schema == nullptr) return schema;
    std::string qualifier = n.alias.value_or(n.table_name);
    for (const auto& col : table_schema->columns) schema.add(qualifier, col.name);
    return schema;
}

struct JoinKeys {
    Expression left_key;
    Expression right_key;
};

JoinKeys resolve_join_keys(const PhysicalPlan& join, const sql::logical::Catalog& schema_catalog) {
    if (join.condition.kind != Expression::Kind::BinaryOp || join.condition.binary_op != BinaryOperator::Eq) {
        throw std::runtime_error("distributed: shuffle join requires a simple equality condition");
    }
    RowSchema left_schema = schema_for_relation(*join.left, schema_catalog);
    RowSchema right_schema = schema_for_relation(*join.right, schema_catalog);

    const Expression* a = join.condition.left.get();
    const Expression* b = join.condition.right.get();
    auto resolves = [](const Expression* e, const RowSchema& s) {
        return e->kind == Expression::Kind::Column && s.resolve(e->table, e->column).has_value();
    };

    if (resolves(a, left_schema) && resolves(b, right_schema)) return JoinKeys{*a, *b};
    if (resolves(b, left_schema) && resolves(a, right_schema)) return JoinKeys{*b, *a};
    throw std::runtime_error("distributed: join condition isn't a simple column-to-column equality across both sides");
}

size_t hash_partition_of(const Literal& v, size_t num_workers) {
    if (num_workers == 0) return 0;
    return sql::util::fnv1a64(sql::execution::literal_to_string(v)) % num_workers;
}

// ── AVG splitting (SUM+COUNT sent to workers, recombined at the coordinator) ──

struct AvgMapping {
    std::string func;
    int direct_idx = -1;
    int sum_idx = -1;
    int count_idx = -1;
};

struct AvgSplit {
    std::vector<AggregateExpr> worker_aggregates;
    std::vector<AvgMapping> mapping; // parallel to the original aggregates list
};

AvgSplit split_avg(const std::vector<AggregateExpr>& original) {
    AvgSplit out;
    for (const auto& agg : original) {
        AvgMapping m;
        m.func = agg.func;
        if (agg.func == "AVG") {
            m.sum_idx = static_cast<int>(out.worker_aggregates.size());
            out.worker_aggregates.push_back(AggregateExpr{"SUM", agg.arg, std::nullopt});
            m.count_idx = static_cast<int>(out.worker_aggregates.size());
            out.worker_aggregates.push_back(AggregateExpr{"COUNT", agg.arg, std::nullopt});
        } else {
            m.direct_idx = static_cast<int>(out.worker_aggregates.size());
            out.worker_aggregates.push_back(agg);
        }
        out.mapping.push_back(m);
    }
    return out;
}

// The column list for merge_partial_aggregates' OUTPUT shape -- the
// original (unsplit) group_by + aggregates layout, not the worker-side
// split layout (which has extra SUM/COUNT columns for every AVG). Mirrors
// execution/operators.cpp's HashAggregateExec constructor naming exactly,
// so a HAVING/ORDER BY re-attached in finish() still resolves against
// these names the same way it would against a real HashAggregateExec.
std::vector<std::string> aggregate_output_columns(const PhysicalPlan& aggregate_node) {
    std::vector<std::string> cols;
    for (size_t i = 0; i < aggregate_node.group_by.size(); ++i) {
        const Expression& e = aggregate_node.group_by[i];
        std::string name = (e.kind == Expression::Kind::Column) ? e.column : ("g" + std::to_string(i));
        std::string qualifier = (e.kind == Expression::Kind::Column && e.table.has_value()) ? *e.table + "." : "";
        cols.push_back(qualifier + name);
    }
    for (size_t i = 0; i < aggregate_node.aggregates.size(); ++i) {
        const auto& agg = aggregate_node.aggregates[i];
        cols.push_back(agg.alias.value_or(agg.func + "_" + std::to_string(i)));
    }
    return cols;
}

std::vector<Row> merge_partial_aggregates(const std::vector<std::vector<Row>>& worker_rows, size_t group_by_count,
                                           const std::vector<AggregateExpr>& original_aggregates,
                                           const AvgSplit& split) {
    struct Acc {
        Row group_key;
        std::vector<double> sum;
        std::vector<double> count;
        std::vector<std::optional<Literal>> min_v;
        std::vector<std::optional<Literal>> max_v;
    };
    std::unordered_map<std::string, Acc> groups;
    std::vector<std::string> order;

    for (const auto& wr : worker_rows) {
        for (const auto& row : wr) {
            Row key(row.begin(), row.begin() + static_cast<long>(group_by_count));
            std::string key_str;
            for (const auto& k : key) {
                key_str += sql::execution::literal_to_string(k);
                key_str += '\x1f';
            }

            auto it = groups.find(key_str);
            if (it == groups.end()) {
                Acc acc;
                acc.group_key = key;
                acc.sum.assign(original_aggregates.size(), 0.0);
                acc.count.assign(original_aggregates.size(), 0.0);
                acc.min_v.assign(original_aggregates.size(), std::nullopt);
                acc.max_v.assign(original_aggregates.size(), std::nullopt);
                it = groups.emplace(key_str, std::move(acc)).first;
                order.push_back(key_str);
            }
            Acc& acc = it->second;

            for (size_t i = 0; i < original_aggregates.size(); ++i) {
                const AvgMapping& m = split.mapping[i];
                const std::string& func = original_aggregates[i].func;
                if (func == "AVG") {
                    acc.sum[i] += sql::execution::literal_as_double(row[group_by_count + static_cast<size_t>(m.sum_idx)]);
                    acc.count[i] +=
                        sql::execution::literal_as_double(row[group_by_count + static_cast<size_t>(m.count_idx)]);
                } else if (func == "SUM") {
                    acc.sum[i] += sql::execution::literal_as_double(row[group_by_count + static_cast<size_t>(m.direct_idx)]);
                } else if (func == "COUNT") {
                    acc.count[i] +=
                        sql::execution::literal_as_double(row[group_by_count + static_cast<size_t>(m.direct_idx)]);
                } else if (func == "MIN") {
                    const Literal& v = row[group_by_count + static_cast<size_t>(m.direct_idx)];
                    if (!acc.min_v[i].has_value() || sql::execution::literal_less(v, *acc.min_v[i])) acc.min_v[i] = v;
                } else if (func == "MAX") {
                    const Literal& v = row[group_by_count + static_cast<size_t>(m.direct_idx)];
                    if (!acc.max_v[i].has_value() || sql::execution::literal_less(*acc.max_v[i], v)) acc.max_v[i] = v;
                }
            }
        }
    }

    std::vector<Row> out;
    for (const auto& key_str : order) {
        Acc& acc = groups.at(key_str);
        Row row = acc.group_key;
        for (size_t i = 0; i < original_aggregates.size(); ++i) {
            const std::string& func = original_aggregates[i].func;
            if (func == "SUM") row.push_back(Literal::floating(acc.sum[i]));
            else if (func == "COUNT") row.push_back(Literal::integer(static_cast<int64_t>(acc.count[i])));
            else if (func == "AVG") row.push_back(Literal::floating(acc.count[i] > 0 ? acc.sum[i] / acc.count[i] : 0.0));
            else if (func == "MIN") row.push_back(acc.min_v[i].value_or(Literal::null()));
            else if (func == "MAX") row.push_back(acc.max_v[i].value_or(Literal::null()));
        }
        out.push_back(std::move(row));
    }
    return out;
}

// ── Worker RPC ─────────────────────────────────────────────────────────────

struct WorkerResponse {
    std::vector<std::string> columns;
    std::vector<Row> rows;
};

std::optional<WorkerResponse> call_worker_execute(const WorkerAddress& w, const PhysicalPlan& plan,
                                                   const std::unordered_map<size_t, std::vector<Row>>& external_rows) {
    JsonValue body = JsonValue::make_object();
    body.object_val["plan"] = sql::util::parse_json(sql::integration::serialize_plan(plan));

    if (!external_rows.empty()) {
        JsonValue ext = JsonValue::make_object();
        for (const auto& [slot, rows] : external_rows) {
            JsonValue slot_obj = JsonValue::make_object();
            slot_obj.object_val["rows"] = rows_to_json(rows);
            ext.object_val[std::to_string(slot)] = std::move(slot_obj);
        }
        body.object_val["external_rows"] = std::move(ext);
    }

    auto resp = http_post_json(w.host, w.port, "/worker/execute", sql::util::to_json(body));
    if (!resp.has_value() || resp->status != 200) return std::nullopt;

    JsonValue parsed = sql::util::parse_json(resp->body);
    WorkerResponse out;
    if (const auto* cols = parsed.find("columns"); cols != nullptr && cols->kind == JsonValue::Kind::Array) {
        for (const auto& c : cols->array_val) out.columns.push_back(c.as_string());
    }
    if (const auto* rows = parsed.find("rows")) out.rows = rows_from_json(*rows);
    return out;
}

// ── Finishing: apply HAVING/Project/Sort/Limit locally over the merged set ──

DistributedQueryResult finish(const UnwrapResult& u, const std::vector<std::string>& result_columns,
                               std::vector<Row> merged_rows, size_t workers_used) {
    PhysicalPlan current = PhysicalPlan::make_external_rows("merged", result_columns, 0, merged_rows.size());
    if (u.aggregate) {
        // These rows are already an aggregate's final output -- reuse the
        // group_by/aggregates fields (otherwise only meaningful on a
        // HashAggregate node) so build_executor's ExternalRows case can
        // register each aggregate the same way a real HashAggregateExec
        // would, and a re-attached HAVING/ORDER BY that re-mentions the
        // aggregate directly (e.g. `HAVING SUM(o.total) > 100`, not
        // through an alias) still resolves. See RowSchema::register_aggregate.
        current.group_by = u.aggregate->group_by;
        current.aggregates = u.aggregate->aggregates;
    }

    if (u.having) {
        current = PhysicalPlan::make_filter(u.having->predicate, std::move(current), cost::Cost{}, 0);
    }
    for (auto it = u.post_ops.rbegin(); it != u.post_ops.rend(); ++it) {
        const PhysicalPlan* op = *it;
        if (op->kind == PhysicalPlan::Kind::Project) {
            current = PhysicalPlan::make_project(op->expressions, std::move(current), cost::Cost{}, 0);
        } else if (op->kind == PhysicalPlan::Kind::Sort) {
            current = PhysicalPlan::make_sort(op->order_by, std::move(current), cost::Cost{}, 0);
        } else if (op->kind == PhysicalPlan::Kind::Limit) {
            current = PhysicalPlan::make_limit(op->count, std::move(current), cost::Cost{}, 0);
        }
    }

    sql::storage::Database dummy; // ExternalRows/Filter/Sort/Limit/Project never touch it
    sql::execution::ExternalRowSets ext = {{0, std::move(merged_rows)}};
    auto executor = sql::execution::build_executor(current, dummy, ext);
    auto exec_result = sql::execution::run_to_completion(*executor);

    DistributedQueryResult out;
    out.distributed = true;
    out.workers_used = workers_used;
    for (size_t i = 0; i < executor->schema().size(); ++i) out.columns.push_back(executor->schema().column_name(i));
    out.rows = std::move(exec_result.rows);
    return out;
}

DistributedQueryResult run_fallback(const PhysicalPlan& plan, const sql::storage::Database& local_full_database,
                                     std::string reason) {
    auto executor = sql::execution::build_executor(plan, local_full_database);
    auto exec_result = sql::execution::run_to_completion(*executor);

    DistributedQueryResult out;
    out.distributed = false;
    out.fallback_reason = std::move(reason);
    out.workers_used = 0;
    for (size_t i = 0; i < executor->schema().size(); ++i) out.columns.push_back(executor->schema().column_name(i));
    out.rows = std::move(exec_result.rows);
    return out;
}

// ── Distributed single-relation (aggregate-or-plain, no join) ──────────────

DistributedQueryResult run_distributed_single(const std::vector<WorkerAddress>& workers, const UnwrapResult& u) {
    std::optional<AvgSplit> split;
    PhysicalPlan worker_plan = *u.core;
    if (u.aggregate) {
        split = split_avg(u.aggregate->aggregates);
        worker_plan =
            PhysicalPlan::make_hash_aggregate(u.aggregate->group_by, split->worker_aggregates, std::move(worker_plan),
                                               cost::Cost{}, 0);
    }

    std::vector<std::vector<Row>> per_worker_rows;
    std::vector<std::string> result_columns;
    for (const auto& w : workers) {
        auto resp = call_worker_execute(w, worker_plan, {});
        if (!resp.has_value()) throw std::runtime_error("distributed: worker unreachable: " + w.host + ":" + std::to_string(w.port));
        per_worker_rows.push_back(std::move(resp->rows));
        result_columns = resp->columns;
    }

    std::vector<Row> merged;
    if (u.aggregate) {
        merged = merge_partial_aggregates(per_worker_rows, u.aggregate->group_by.size(), u.aggregate->aggregates, *split);
        result_columns = aggregate_output_columns(*u.aggregate); // post-merge shape, not the worker's split-AVG shape
    } else {
        for (auto& wr : per_worker_rows) merged.insert(merged.end(), wr.begin(), wr.end());
    }

    return finish(u, result_columns, std::move(merged), workers.size());
}

// ── Distributed join: broadcast ─────────────────────────────────────────────

DistributedQueryResult run_broadcast_join(const std::vector<WorkerAddress>& workers, const PhysicalPlan& join,
                                           bool broadcast_left, const UnwrapResult& u) {
    const PhysicalPlan& small_side = broadcast_left ? *join.left : *join.right;
    const PhysicalPlan& large_side = broadcast_left ? *join.right : *join.left;

    std::vector<Row> broadcast_rows;
    std::vector<std::string> broadcast_columns;
    for (const auto& w : workers) {
        auto resp = call_worker_execute(w, small_side, {});
        if (!resp.has_value()) {
            throw std::runtime_error("distributed: worker unreachable during broadcast gather: " + w.host + ":" +
                                      std::to_string(w.port));
        }
        broadcast_rows.insert(broadcast_rows.end(), resp->rows.begin(), resp->rows.end());
        broadcast_columns = resp->columns;
    }

    PhysicalPlan ext = PhysicalPlan::make_external_rows("broadcast:" + small_side.table_name, broadcast_columns, 0,
                                                         broadcast_rows.size());
    PhysicalPlan worker_join =
        broadcast_left ? PhysicalPlan::make_join(join.kind, join.join_type, join.condition, std::move(ext), large_side,
                                                  cost::Cost{}, 0)
                       : PhysicalPlan::make_join(join.kind, join.join_type, join.condition, large_side, std::move(ext),
                                                  cost::Cost{}, 0);

    std::optional<AvgSplit> split;
    if (u.aggregate) {
        split = split_avg(u.aggregate->aggregates);
        worker_join = PhysicalPlan::make_hash_aggregate(u.aggregate->group_by, split->worker_aggregates,
                                                          std::move(worker_join), cost::Cost{}, 0);
    }

    std::vector<std::vector<Row>> per_worker_rows;
    std::vector<std::string> result_columns;
    for (const auto& w : workers) {
        auto resp = call_worker_execute(w, worker_join, {{0, broadcast_rows}});
        if (!resp.has_value()) throw std::runtime_error("distributed: worker unreachable: " + w.host + ":" + std::to_string(w.port));
        per_worker_rows.push_back(std::move(resp->rows));
        result_columns = resp->columns;
    }

    std::vector<Row> merged;
    if (u.aggregate) {
        merged = merge_partial_aggregates(per_worker_rows, u.aggregate->group_by.size(), u.aggregate->aggregates, *split);
        result_columns = aggregate_output_columns(*u.aggregate); // post-merge shape, not the worker's split-AVG shape
    } else {
        for (auto& wr : per_worker_rows) merged.insert(merged.end(), wr.begin(), wr.end());
    }

    return finish(u, result_columns, std::move(merged), workers.size());
}

// ── Distributed join: shuffle ────────────────────────────────────────────────

DistributedQueryResult run_shuffle_join(const std::vector<WorkerAddress>& workers, const PhysicalPlan& join,
                                         const UnwrapResult& u, const sql::logical::Catalog& schema_catalog) {
    JoinKeys keys = resolve_join_keys(join, schema_catalog);

    size_t n = workers.size();
    std::vector<std::vector<Row>> left_by_target(n), right_by_target(n);
    std::vector<std::string> left_columns, right_columns;

    for (const auto& w : workers) {
        auto left_resp = call_worker_execute(w, *join.left, {});
        auto right_resp = call_worker_execute(w, *join.right, {});
        if (!left_resp.has_value() || !right_resp.has_value()) {
            throw std::runtime_error("distributed: worker unreachable during shuffle gather: " + w.host + ":" +
                                      std::to_string(w.port));
        }
        left_columns = left_resp->columns;
        right_columns = right_resp->columns;

        RowSchema left_schema = schema_for_relation(*join.left, schema_catalog);
        RowSchema right_schema = schema_for_relation(*join.right, schema_catalog);
        auto left_key_idx = left_schema.resolve(keys.left_key.table, keys.left_key.column);
        auto right_key_idx = right_schema.resolve(keys.right_key.table, keys.right_key.column);
        if (!left_key_idx.has_value() || !right_key_idx.has_value()) {
            throw std::runtime_error("distributed: could not resolve join key column for shuffle");
        }

        for (auto& row : left_resp->rows) {
            size_t target = hash_partition_of(row[*left_key_idx], n);
            left_by_target[target].push_back(std::move(row));
        }
        for (auto& row : right_resp->rows) {
            size_t target = hash_partition_of(row[*right_key_idx], n);
            right_by_target[target].push_back(std::move(row));
        }
    }

    PhysicalPlan left_ext = PhysicalPlan::make_external_rows("shuffle:" + unwrap_filter(*join.left).table_name,
                                                              left_columns, 0, 0);
    PhysicalPlan right_ext = PhysicalPlan::make_external_rows("shuffle:" + unwrap_filter(*join.right).table_name,
                                                               right_columns, 1, 0);
    PhysicalPlan worker_join =
        PhysicalPlan::make_join(join.kind, join.join_type, join.condition, std::move(left_ext), std::move(right_ext),
                                 cost::Cost{}, 0);

    std::optional<AvgSplit> split;
    if (u.aggregate) {
        split = split_avg(u.aggregate->aggregates);
        worker_join = PhysicalPlan::make_hash_aggregate(u.aggregate->group_by, split->worker_aggregates,
                                                          std::move(worker_join), cost::Cost{}, 0);
    }

    std::vector<std::vector<Row>> per_worker_rows;
    std::vector<std::string> result_columns;
    for (size_t i = 0; i < n; ++i) {
        std::unordered_map<size_t, std::vector<Row>> ext_rows = {{0, std::move(left_by_target[i])},
                                                                   {1, std::move(right_by_target[i])}};
        auto resp = call_worker_execute(workers[i], worker_join, ext_rows);
        if (!resp.has_value()) {
            throw std::runtime_error("distributed: worker unreachable: " + workers[i].host + ":" +
                                      std::to_string(workers[i].port));
        }
        per_worker_rows.push_back(std::move(resp->rows));
        result_columns = resp->columns;
    }

    std::vector<Row> merged;
    if (u.aggregate) {
        merged = merge_partial_aggregates(per_worker_rows, u.aggregate->group_by.size(), u.aggregate->aggregates, *split);
        result_columns = aggregate_output_columns(*u.aggregate); // post-merge shape, not the worker's split-AVG shape
    } else {
        for (auto& wr : per_worker_rows) merged.insert(merged.end(), wr.begin(), wr.end());
    }

    return finish(u, result_columns, std::move(merged), n);
}

} // namespace

DistributedQueryResult run_distributed_query(const std::string& sql, const sql::logical::Catalog& schema_catalog,
                                              const sql::statistics::StatisticsCatalog& stats_catalog,
                                              const sql::storage::Database& local_full_database,
                                              const std::vector<WorkerAddress>& workers,
                                              sql::integration::CacheClient& cache,
                                              sql::integration::CacheVersions versions) {
    if (workers.empty()) throw std::runtime_error("distributed: no workers configured");

    auto start = std::chrono::steady_clock::now();

    auto planned = sql::integration::plan_with_cache(sql, schema_catalog, stats_catalog, cache, versions);
    UnwrapResult u = unwrap(planned.plan);

    DistributedQueryResult result;
    if (is_join(*u.core)) {
        if (!is_single_relation(*u.core->left) || !is_single_relation(*u.core->right)) {
            result = run_fallback(planned.plan, local_full_database,
                                   "join has a side that isn't a single base relation (e.g. a 3+ table join) -- "
                                   "distributing it would need an exchange at more than one point in the tree, "
                                   "which needs physical property tracking (see ROADMAP.md), not built yet");
        } else {
            size_t left_est = u.core->left->estimated_rows;
            size_t right_est = u.core->right->estimated_rows;
            bool can_broadcast_left = left_est <= kBroadcastThreshold;
            bool can_broadcast_right = right_est <= kBroadcastThreshold;
            if (can_broadcast_left || can_broadcast_right) {
                bool broadcast_left = can_broadcast_left && (!can_broadcast_right || left_est <= right_est);
                result = run_broadcast_join(workers, *u.core, broadcast_left, u);
            } else {
                result = run_shuffle_join(workers, *u.core, u, schema_catalog);
            }
        }
    } else if (is_single_relation(*u.core)) {
        result = run_distributed_single(workers, u);
    } else {
        result = run_fallback(planned.plan, local_full_database,
                               "plan's core isn't a single base relation or a 2-relation join -- not eligible for "
                               "this build's distributed execution");
    }

    result.total_ms = ms_since(start);
    return result;
}

} // namespace sql::distributed
