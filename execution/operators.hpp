#pragma once

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "../logical/logical_plan.hpp" // AggregateExpr
#include "../parser/ast.hpp"
#include "../storage/database.hpp"
#include "executor.hpp"

namespace sql::execution {

class SeqScanExec : public Executor {
public:
    SeqScanExec(std::string table_name, std::optional<std::string> alias, const sql::storage::Table& table);

    const RowSchema& schema() const override { return schema_; }
    std::string operator_name() const override { return "SeqScan(" + table_name_ + ")"; }
    std::vector<Executor*> children() const override { return {}; }

protected:
    void open_impl() override;
    std::optional<sql::storage::Row> next_impl() override;
    void close_impl() override {}

private:
    std::string table_name_;
    const sql::storage::Table& table_;
    RowSchema schema_;
    size_t pos_ = 0;
};

// Functionally equivalent to SeqScan + an equality Filter -- there's no
// real index structure over the in-memory CSV tables yet, so this doesn't
// execute any faster than a scan+filter would. It exists so an IndexScan
// node's *output* can be verified correct; its actual latency doesn't yet
// reflect why the optimizer preferred it over SeqScan. See ROADMAP.md.
class IndexScanExec : public Executor {
public:
    IndexScanExec(std::string table_name, std::optional<std::string> alias, const sql::storage::Table& table,
                   std::string index_column, sql::parser::Expression probe_value);

    const RowSchema& schema() const override { return schema_; }
    std::string operator_name() const override { return "IndexScan(" + table_name_ + ")"; }
    std::vector<Executor*> children() const override { return {}; }

protected:
    void open_impl() override;
    std::optional<sql::storage::Row> next_impl() override;
    void close_impl() override {}

private:
    std::string table_name_;
    const sql::storage::Table& table_;
    RowSchema schema_;
    std::string index_column_;
    sql::parser::Expression probe_value_;
    size_t pos_ = 0;
    sql::parser::Literal probe_cached_;
    size_t col_idx_cached_ = 0;
};

class FilterExec : public Executor {
public:
    FilterExec(sql::parser::Expression predicate, std::unique_ptr<Executor> input);

    const RowSchema& schema() const override { return input_->schema(); }
    std::string operator_name() const override { return "Filter"; }
    std::vector<Executor*> children() const override { return {input_.get()}; }

protected:
    void open_impl() override { input_->open(); }
    std::optional<sql::storage::Row> next_impl() override;
    void close_impl() override { input_->close(); }

private:
    sql::parser::Expression predicate_;
    std::unique_ptr<Executor> input_;
};

// NestedLoopJoin and IndexNestedLoopJoin both execute as this class -- see
// the IndexScanExec note above; the same "no real index yet" simplification
// applies to the nested-loop-with-index-probe strategy.
class NestedLoopJoinExec : public Executor {
public:
    NestedLoopJoinExec(sql::parser::Expression condition, std::unique_ptr<Executor> left, std::unique_ptr<Executor> right);

    const RowSchema& schema() const override { return schema_; }
    std::string operator_name() const override { return "NestedLoopJoin"; }
    std::vector<Executor*> children() const override { return {left_.get(), right_.get()}; }

protected:
    void open_impl() override;
    std::optional<sql::storage::Row> next_impl() override;
    void close_impl() override;

private:
    sql::parser::Expression condition_;
    std::unique_ptr<Executor> left_;
    std::unique_ptr<Executor> right_;
    RowSchema schema_;
    std::vector<sql::storage::Row> right_rows_; // materialized once per open()
    std::optional<sql::storage::Row> current_left_;
    size_t right_pos_ = 0;
    bool left_exhausted_ = false;

    bool advance_left();
};

class HashJoinExec : public Executor {
public:
    HashJoinExec(sql::parser::Expression condition, std::unique_ptr<Executor> left, std::unique_ptr<Executor> right);

    const RowSchema& schema() const override { return schema_; }
    std::string operator_name() const override { return "HashJoin"; }
    std::vector<Executor*> children() const override { return {left_.get(), right_.get()}; }

protected:
    void open_impl() override;
    std::optional<sql::storage::Row> next_impl() override;
    void close_impl() override;

private:
    sql::parser::Expression condition_;
    std::unique_ptr<Executor> left_;
    std::unique_ptr<Executor> right_;
    RowSchema schema_;
    const sql::parser::Expression* left_key_ = nullptr;  // resolves against left_->schema()
    const sql::parser::Expression* right_key_ = nullptr; // resolves against right_->schema()
    std::unordered_multimap<std::string, sql::storage::Row> build_; // right rows, keyed by join key -- always builds on the right
    std::optional<sql::storage::Row> current_left_;
    std::unordered_multimap<std::string, sql::storage::Row>::iterator probe_it_, probe_end_;
    bool probing_ = false;
};

class HashAggregateExec : public Executor {
public:
    HashAggregateExec(std::vector<sql::parser::Expression> group_by, std::vector<sql::logical::AggregateExpr> aggregates,
                       std::unique_ptr<Executor> input);

    const RowSchema& schema() const override { return schema_; }
    std::string operator_name() const override { return "HashAggregate"; }
    std::vector<Executor*> children() const override { return {input_.get()}; }

protected:
    void open_impl() override;
    std::optional<sql::storage::Row> next_impl() override;
    void close_impl() override { input_->close(); }

private:
    std::vector<sql::parser::Expression> group_by_;
    std::vector<sql::logical::AggregateExpr> aggregates_;
    std::unique_ptr<Executor> input_;
    RowSchema schema_;
    std::vector<sql::storage::Row> results_;
    size_t pos_ = 0;
};

class ProjectExec : public Executor {
public:
    ProjectExec(std::vector<std::pair<sql::parser::Expression, std::optional<std::string>>> expressions,
                std::unique_ptr<Executor> input);

    const RowSchema& schema() const override { return schema_; }
    std::string operator_name() const override { return "Project"; }
    std::vector<Executor*> children() const override { return {input_.get()}; }

protected:
    void open_impl() override { input_->open(); }
    std::optional<sql::storage::Row> next_impl() override;
    void close_impl() override { input_->close(); }

private:
    std::vector<std::pair<sql::parser::Expression, std::optional<std::string>>> expressions_;
    std::unique_ptr<Executor> input_;
    RowSchema schema_;
};

class SortExec : public Executor {
public:
    SortExec(std::vector<sql::parser::OrderByItem> order_by, std::unique_ptr<Executor> input);

    const RowSchema& schema() const override { return input_->schema(); }
    std::string operator_name() const override { return "Sort"; }
    std::vector<Executor*> children() const override { return {input_.get()}; }

protected:
    void open_impl() override;
    std::optional<sql::storage::Row> next_impl() override;
    void close_impl() override { input_->close(); }

private:
    std::vector<sql::parser::OrderByItem> order_by_;
    std::unique_ptr<Executor> input_;
    std::vector<sql::storage::Row> rows_;
    size_t pos_ = 0;
};

class LimitExec : public Executor {
public:
    LimitExec(size_t count, std::unique_ptr<Executor> input);

    const RowSchema& schema() const override { return input_->schema(); }
    std::string operator_name() const override { return "Limit"; }
    std::vector<Executor*> children() const override { return {input_.get()}; }

protected:
    void open_impl() override {
        input_->open();
        emitted_ = 0;
    }
    std::optional<sql::storage::Row> next_impl() override;
    void close_impl() override { input_->close(); }

private:
    size_t count_;
    std::unique_ptr<Executor> input_;
    size_t emitted_ = 0;
};

// A leaf that serves a pre-supplied row set instead of scanning a table --
// how a distributed worker receives a broadcast or shuffled input (see
// PhysicalPlan::Kind::ExternalRows, distributed/coordinator.cpp). Rows are
// copied in at construction; this executor owns nothing beyond that.
class ExternalRowsExec : public Executor {
public:
    ExternalRowsExec(std::string table_name, RowSchema schema, std::vector<sql::storage::Row> rows)
        : table_name_(std::move(table_name)), schema_(std::move(schema)), rows_(std::move(rows)) {}

    const RowSchema& schema() const override { return schema_; }
    std::string operator_name() const override { return "ExternalRows(" + table_name_ + ")"; }
    std::vector<Executor*> children() const override { return {}; }

protected:
    void open_impl() override { pos_ = 0; }
    std::optional<sql::storage::Row> next_impl() override {
        if (pos_ >= rows_.size()) return std::nullopt;
        return rows_[pos_++];
    }
    void close_impl() override {}

private:
    std::string table_name_;
    RowSchema schema_;
    std::vector<sql::storage::Row> rows_;
    size_t pos_ = 0;
};

} // namespace sql::execution
