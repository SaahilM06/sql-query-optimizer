#include "access_path_generator.hpp"

#include <algorithm>
#include <cmath>
#include <optional>

#include "../physical/cost.hpp"

namespace sql::optimizer {

using namespace sql::parser;
using namespace sql::physical;
using namespace sql::statistics;
using sql::logical::LogicalPlan;
using sql::logical::TableSchema;

namespace {

size_t rows_to_size(double rows) { return static_cast<size_t>(std::max(1.0, std::round(rows))); }

// Finds a local_filter that's an equality (Column = Literal, either
// order) on an indexed column, if any. Returns its index into
// rel.local_filters.
std::optional<size_t> find_indexed_equality(const Relation& rel, const TableSchema& schema) {
    for (size_t i = 0; i < rel.local_filters.size(); ++i) {
        const Expression& pred = rel.local_filters[i];
        if (pred.kind != Expression::Kind::BinaryOp || pred.binary_op != BinaryOperator::Eq) continue;

        const Expression* col_side = nullptr;
        if (pred.left->kind == Expression::Kind::Column && pred.right->kind == Expression::Kind::Literal) {
            col_side = pred.left.get();
        } else if (pred.right->kind == Expression::Kind::Column && pred.left->kind == Expression::Kind::Literal) {
            col_side = pred.right.get();
        } else {
            continue;
        }

        if (schema.has_index_on(col_side->column)) return i;
    }
    return std::nullopt;
}

const Expression& literal_side_of(const Expression& pred) {
    return pred.left->kind == Expression::Kind::Literal ? *pred.left : *pred.right;
}
const Expression& column_side_of(const Expression& pred) {
    return pred.left->kind == Expression::Kind::Column ? *pred.left : *pred.right;
}

} // namespace

PhysicalPlan AccessPathGenerator::best_access_path(const Relation& rel) const {
    Estimate scan_est = cardinality_.estimate_scan(rel.table_name);
    size_t base_rows = rows_to_size(scan_est.rows);

    const auto* stats = stats_catalog_.get(rel.table_name);
    double page_count =
        (stats != nullptr && stats->page_count > 0.0) ? stats->page_count : cost::pages_for(base_rows, 64);

    // A synthetic single-relation "scope" so CardinalityEstimator can
    // resolve statistics for each local_filter the same way it would for
    // an ordinary Filter-over-Scan outside the join-search path.
    LogicalPlan scope = LogicalPlan::make_scan(rel.table_name, rel.alias);

    // ── Candidate 1: SeqScan, every local_filter applied as a stacked Filter ──
    PhysicalPlan seq = PhysicalPlan::make_seq_scan(rel.table_name, rel.alias, {}, cost::seq_scan(base_rows, page_count),
                                                    base_rows, scan_est.reasoning, scan_est.confidence);

    double running_rows = scan_est.rows;
    for (const auto& pred : rel.local_filters) {
        Estimate filter_est = cardinality_.estimate_filter(running_rows, pred, scope);
        size_t rows = rows_to_size(filter_est.rows);
        cost::Cost c = seq.estimated_cost + cost::filter(rows_to_size(running_rows));
        seq = PhysicalPlan::make_filter(pred, std::move(seq), c, rows, filter_est.reasoning, filter_est.confidence);
        running_rows = filter_est.rows;
    }

    // ── Candidate 2: IndexScan on one equality filter, remaining filters as Filters ──
    const auto* schema = schema_catalog_.get(rel.table_name);
    if (schema == nullptr) return seq;

    auto index_pos = find_indexed_equality(rel, *schema);
    if (!index_pos.has_value()) return seq;

    const Expression& index_pred = rel.local_filters[*index_pos];
    Estimate index_filter_est = cardinality_.estimate_filter(scan_est.rows, index_pred, scope);
    size_t matched_rows = rows_to_size(index_filter_est.rows);

    PhysicalPlan index_scan =
        PhysicalPlan::make_index_scan(rel.table_name, rel.alias, {}, column_side_of(index_pred).column,
                                       literal_side_of(index_pred), cost::index_scan(matched_rows), matched_rows,
                                       index_filter_est.reasoning, index_filter_est.confidence);

    double idx_running_rows = index_filter_est.rows;
    for (size_t i = 0; i < rel.local_filters.size(); ++i) {
        if (i == *index_pos) continue;
        const Expression& pred = rel.local_filters[i];
        Estimate filter_est = cardinality_.estimate_filter(idx_running_rows, pred, scope);
        size_t rows = rows_to_size(filter_est.rows);
        cost::Cost c = index_scan.estimated_cost + cost::filter(rows_to_size(idx_running_rows));
        index_scan =
            PhysicalPlan::make_filter(pred, std::move(index_scan), c, rows, filter_est.reasoning, filter_est.confidence);
        idx_running_rows = filter_est.rows;
    }

    return (index_scan.estimated_cost.total() < seq.estimated_cost.total()) ? std::move(index_scan) : std::move(seq);
}

} // namespace sql::optimizer
