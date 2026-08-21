#include "operators.hpp"

#include <algorithm>
#include <stdexcept>
#include <unordered_map>

#include "expr_eval.hpp"
#include "value_ops.hpp"

namespace sql::execution {

using namespace sql::parser;
using sql::logical::AggregateExpr;
using sql::storage::Row;

namespace {

// Column expressions get their own name; anything else (an arithmetic
// expression with no alias, say) gets a synthetic placeholder -- SQL would
// normally require an alias in that case, but this keeps evaluation from
// throwing over a cosmetic column-naming gap.
std::string expr_display_name(const Expression& e, size_t fallback_index, const char* fallback_prefix) {
    if (e.kind == Expression::Kind::Column) return e.column;
    return std::string(fallback_prefix) + std::to_string(fallback_index);
}

// The table qualifier to preserve on a GROUP BY output column, if any. A
// later ORDER BY/HAVING may still reference `o.customer_id` with its
// original qualifier even after aggregation collapses the row -- the
// planner doesn't strip or rewrite that qualifier (see
// LogicalPlanner::plan), so the aggregate's output schema has to keep it
// too, or resolution against it fails.
std::optional<std::string> expr_display_table(const Expression& e) {
    if (e.kind == Expression::Kind::Column) return e.table;
    return std::nullopt;
}

} // namespace

// ── SeqScanExec ────────────────────────────────────────────────────────────

SeqScanExec::SeqScanExec(std::string table_name, std::optional<std::string> alias, const sql::storage::Table& table)
    : table_name_(std::move(table_name)), table_(table) {
    std::string qualifier = alias.value_or(table_name_);
    for (const auto& col : table_.column_names()) schema_.add(qualifier, col);
}

void SeqScanExec::open_impl() { pos_ = 0; }

std::optional<Row> SeqScanExec::next_impl() {
    if (pos_ >= table_.rows().size()) return std::nullopt;
    return table_.rows()[pos_++];
}

// ── IndexScanExec ──────────────────────────────────────────────────────────

IndexScanExec::IndexScanExec(std::string table_name, std::optional<std::string> alias, const sql::storage::Table& table,
                              std::string index_column, Expression probe_value)
    : table_name_(std::move(table_name)),
      table_(table),
      index_column_(std::move(index_column)),
      probe_value_(std::move(probe_value)) {
    std::string qualifier = alias.value_or(table_name_);
    for (const auto& col : table_.column_names()) schema_.add(qualifier, col);
}

void IndexScanExec::open_impl() {
    pos_ = 0;
    probe_cached_ = evaluate(probe_value_, Row{}, RowSchema{});
    auto idx = schema_.resolve(std::nullopt, index_column_);
    if (!idx.has_value()) throw std::runtime_error("execution: index column '" + index_column_ + "' not found");
    col_idx_cached_ = *idx;
}

std::optional<Row> IndexScanExec::next_impl() {
    while (pos_ < table_.rows().size()) {
        const Row& row = table_.rows()[pos_++];
        if (literal_equal(row[col_idx_cached_], probe_cached_)) return row;
    }
    return std::nullopt;
}

// ── FilterExec ─────────────────────────────────────────────────────────────

FilterExec::FilterExec(Expression predicate, std::unique_ptr<Executor> input)
    : predicate_(std::move(predicate)), input_(std::move(input)) {}

std::optional<Row> FilterExec::next_impl() {
    for (;;) {
        auto row = input_->next();
        if (!row.has_value()) return std::nullopt;
        if (literal_truthy(evaluate(predicate_, *row, input_->schema()))) return row;
    }
}

// ── NestedLoopJoinExec ─────────────────────────────────────────────────────

NestedLoopJoinExec::NestedLoopJoinExec(Expression condition, std::unique_ptr<Executor> left, std::unique_ptr<Executor> right)
    : condition_(std::move(condition)), left_(std::move(left)), right_(std::move(right)) {
    schema_ = left_->schema();
    schema_.extend(right_->schema());
}

void NestedLoopJoinExec::open_impl() {
    left_->open();
    right_->open();
    right_rows_.clear();
    for (;;) {
        auto r = right_->next();
        if (!r.has_value()) break;
        right_rows_.push_back(*r);
    }
    right_pos_ = 0;
    left_exhausted_ = false;
    current_left_ = std::nullopt;
}

void NestedLoopJoinExec::close_impl() {
    left_->close();
    right_->close();
}

bool NestedLoopJoinExec::advance_left() {
    auto l = left_->next();
    if (!l.has_value()) {
        left_exhausted_ = true;
        return false;
    }
    current_left_ = l;
    right_pos_ = 0;
    return true;
}

std::optional<Row> NestedLoopJoinExec::next_impl() {
    if (!current_left_.has_value() && !left_exhausted_) {
        if (!advance_left()) return std::nullopt;
    }
    while (!left_exhausted_) {
        while (right_pos_ < right_rows_.size()) {
            Row combined = *current_left_;
            const Row& r = right_rows_[right_pos_++];
            combined.insert(combined.end(), r.begin(), r.end());
            if (literal_truthy(evaluate(condition_, combined, schema_))) return combined;
        }
        if (!advance_left()) return std::nullopt;
    }
    return std::nullopt;
}

// ── HashJoinExec ───────────────────────────────────────────────────────────

HashJoinExec::HashJoinExec(Expression condition, std::unique_ptr<Executor> left, std::unique_ptr<Executor> right)
    : condition_(std::move(condition)), left_(std::move(left)), right_(std::move(right)) {
    schema_ = left_->schema();
    schema_.extend(right_->schema());

    if (condition_.kind != Expression::Kind::BinaryOp || condition_.binary_op != BinaryOperator::Eq) {
        throw std::runtime_error("execution: HashJoin requires an equality condition");
    }
    const Expression* a = condition_.left.get();
    const Expression* b = condition_.right.get();
    bool a_is_left = a->kind == Expression::Kind::Column && left_->schema().resolve(a->table, a->column).has_value();
    bool a_is_right = a->kind == Expression::Kind::Column && right_->schema().resolve(a->table, a->column).has_value();
    bool b_is_left = b->kind == Expression::Kind::Column && left_->schema().resolve(b->table, b->column).has_value();
    bool b_is_right = b->kind == Expression::Kind::Column && right_->schema().resolve(b->table, b->column).has_value();

    if (a_is_left && b_is_right) {
        left_key_ = a;
        right_key_ = b;
    } else if (a_is_right && b_is_left) {
        left_key_ = b;
        right_key_ = a;
    } else {
        throw std::runtime_error("execution: HashJoin condition must be a simple column-to-column equality across both sides");
    }
}

void HashJoinExec::open_impl() {
    left_->open();
    right_->open();
    build_.clear();
    for (;;) {
        auto r = right_->next();
        if (!r.has_value()) break;
        Literal key = evaluate(*right_key_, *r, right_->schema());
        build_.emplace(literal_to_string(key), *r);
    }
    current_left_ = std::nullopt;
    probing_ = false;
}

void HashJoinExec::close_impl() {
    left_->close();
    right_->close();
}

std::optional<Row> HashJoinExec::next_impl() {
    for (;;) {
        if (probing_) {
            if (probe_it_ != probe_end_) {
                Row combined = *current_left_;
                const Row& r = probe_it_->second;
                combined.insert(combined.end(), r.begin(), r.end());
                ++probe_it_;
                return combined;
            }
            probing_ = false;
        }
        auto l = left_->next();
        if (!l.has_value()) return std::nullopt;
        current_left_ = l;
        Literal key = evaluate(*left_key_, *current_left_, left_->schema());
        auto range = build_.equal_range(literal_to_string(key));
        probe_it_ = range.first;
        probe_end_ = range.second;
        probing_ = true;
    }
}

// ── HashAggregateExec ──────────────────────────────────────────────────────

namespace {
struct AccState {
    Row group_key;
    std::vector<double> sums;
    std::vector<size_t> counts;
    std::vector<std::optional<Literal>> mins;
    std::vector<std::optional<Literal>> maxs;
};
} // namespace

HashAggregateExec::HashAggregateExec(std::vector<Expression> group_by, std::vector<AggregateExpr> aggregates,
                                      std::unique_ptr<Executor> input)
    : group_by_(std::move(group_by)), aggregates_(std::move(aggregates)), input_(std::move(input)) {
    for (size_t i = 0; i < group_by_.size(); ++i) {
        schema_.add(expr_display_table(group_by_[i]), expr_display_name(group_by_[i], i, "g"));
    }
    for (size_t i = 0; i < aggregates_.size(); ++i) {
        std::string name = aggregates_[i].alias.value_or(aggregates_[i].func + "_" + std::to_string(i));
        size_t idx = schema_.size();
        schema_.add(std::nullopt, name);
        schema_.register_aggregate(aggregates_[i].func, aggregates_[i].arg, idx);
    }
}

void HashAggregateExec::open_impl() {
    input_->open();
    results_.clear();
    pos_ = 0;

    std::unordered_map<std::string, AccState> groups;
    std::vector<std::string> group_order; // first-seen order, for deterministic output

    for (;;) {
        auto row = input_->next();
        if (!row.has_value()) break;

        Row key;
        key.reserve(group_by_.size());
        for (const auto& g : group_by_) key.push_back(evaluate(g, *row, input_->schema()));

        std::string key_str;
        for (const auto& k : key) {
            key_str += literal_to_string(k);
            key_str += '\x1f';
        }

        auto it = groups.find(key_str);
        if (it == groups.end()) {
            AccState st;
            st.group_key = key;
            st.sums.assign(aggregates_.size(), 0.0);
            st.counts.assign(aggregates_.size(), 0);
            st.mins.assign(aggregates_.size(), std::nullopt);
            st.maxs.assign(aggregates_.size(), std::nullopt);
            it = groups.emplace(key_str, std::move(st)).first;
            group_order.push_back(key_str);
        }
        AccState& st = it->second;

        for (size_t i = 0; i < aggregates_.size(); ++i) {
            const AggregateExpr& agg = aggregates_[i];
            if (agg.func == "COUNT") {
                if (agg.arg.kind == Expression::Kind::Wildcard) {
                    ++st.counts[i];
                } else {
                    Literal v = evaluate(agg.arg, *row, input_->schema());
                    if (!literal_is_null(v)) ++st.counts[i];
                }
                continue;
            }
            Literal v = evaluate(agg.arg, *row, input_->schema());
            if (literal_is_null(v)) continue;
            if (agg.func == "SUM" || agg.func == "AVG") {
                st.sums[i] += literal_as_double(v);
                ++st.counts[i];
            } else if (agg.func == "MIN") {
                if (!st.mins[i].has_value() || literal_less(v, *st.mins[i])) st.mins[i] = v;
            } else if (agg.func == "MAX") {
                if (!st.maxs[i].has_value() || literal_less(*st.maxs[i], v)) st.maxs[i] = v;
            } else {
                throw std::runtime_error("execution: unknown aggregate function '" + agg.func + "'");
            }
        }
    }

    for (const auto& key_str : group_order) {
        const AccState& st = groups.at(key_str);
        Row out = st.group_key;
        for (size_t i = 0; i < aggregates_.size(); ++i) {
            const std::string& func = aggregates_[i].func;
            if (func == "COUNT") out.push_back(Literal::integer(static_cast<int64_t>(st.counts[i])));
            else if (func == "SUM") out.push_back(Literal::floating(st.sums[i]));
            else if (func == "AVG")
                out.push_back(Literal::floating(st.counts[i] > 0 ? st.sums[i] / static_cast<double>(st.counts[i]) : 0.0));
            else if (func == "MIN") out.push_back(st.mins[i].value_or(Literal::null()));
            else if (func == "MAX") out.push_back(st.maxs[i].value_or(Literal::null()));
        }
        results_.push_back(std::move(out));
    }

    // No GROUP BY at all -> always exactly one output row, even over zero
    // input rows (COUNT(*) over an empty result should read 0, not produce
    // no rows at all).
    if (group_by_.empty() && results_.empty()) {
        Row out;
        for (const auto& agg : aggregates_) {
            if (agg.func == "COUNT") out.push_back(Literal::integer(0));
            else if (agg.func == "SUM") out.push_back(Literal::floating(0.0));
            else out.push_back(Literal::null());
        }
        results_.push_back(std::move(out));
    }
}

std::optional<Row> HashAggregateExec::next_impl() {
    if (pos_ >= results_.size()) return std::nullopt;
    return results_[pos_++];
}

// ── ProjectExec ────────────────────────────────────────────────────────────

ProjectExec::ProjectExec(std::vector<std::pair<Expression, std::optional<std::string>>> expressions,
                          std::unique_ptr<Executor> input)
    : expressions_(std::move(expressions)), input_(std::move(input)) {
    for (size_t i = 0; i < expressions_.size(); ++i) {
        const auto& [expr, alias] = expressions_[i];
        schema_.add(std::nullopt, alias.value_or(expr_display_name(expr, i, "expr")));
    }
}

std::optional<Row> ProjectExec::next_impl() {
    auto row = input_->next();
    if (!row.has_value()) return std::nullopt;
    Row out;
    out.reserve(expressions_.size());
    for (const auto& [expr, alias] : expressions_) {
        (void)alias;
        out.push_back(evaluate(expr, *row, input_->schema()));
    }
    return out;
}

// ── SortExec ───────────────────────────────────────────────────────────────

SortExec::SortExec(std::vector<OrderByItem> order_by, std::unique_ptr<Executor> input)
    : order_by_(std::move(order_by)), input_(std::move(input)) {}

void SortExec::open_impl() {
    input_->open();
    rows_.clear();
    for (;;) {
        auto row = input_->next();
        if (!row.has_value()) break;
        rows_.push_back(*row);
    }
    std::sort(rows_.begin(), rows_.end(), [&](const Row& a, const Row& b) {
        for (const auto& item : order_by_) {
            Literal va = evaluate(item.expression, a, input_->schema());
            Literal vb = evaluate(item.expression, b, input_->schema());
            if (literal_equal(va, vb)) continue;
            bool less = literal_less(va, vb);
            return item.ascending ? less : !less;
        }
        return false;
    });
    pos_ = 0;
}

std::optional<Row> SortExec::next_impl() {
    if (pos_ >= rows_.size()) return std::nullopt;
    return rows_[pos_++];
}

// ── LimitExec ──────────────────────────────────────────────────────────────

LimitExec::LimitExec(size_t count, std::unique_ptr<Executor> input) : count_(count), input_(std::move(input)) {}

std::optional<Row> LimitExec::next_impl() {
    if (emitted_ >= count_) return std::nullopt;
    auto row = input_->next();
    if (!row.has_value()) return std::nullopt;
    ++emitted_;
    return row;
}

} // namespace sql::execution
