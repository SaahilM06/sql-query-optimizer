#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <vector>

#include "../storage/row.hpp"
#include "row_schema.hpp"

namespace sql::execution {

// Pull-iterator execution model: open() prepares state, next() returns one
// row at a time (nullopt when exhausted), close() releases anything open()
// acquired.
//
// open()/next() are non-virtual public wrappers around open_impl()/
// next_impl() that time themselves and accumulate into elapsed_ms_ --
// deliberately timing *both*, not just next(), because a materializing
// operator (HashJoin's build phase, Sort's full drain-and-sort,
// HashAggregate's full drain-and-group) does its real work inside
// open_impl(), not spread across next_impl() calls. Since a parent's
// open_impl()/next_impl() calls its children through these same timed
// wrappers, each node's elapsed_ms ends up cumulative (self + every
// descendant) purely from normal call nesting, with no manual bookkeeping
// -- matching how PhysicalPlan's own estimated_cost/estimated_rows are
// already cumulative per node, so EXPLAIN ANALYZE can compare the two
// directly at every level, not just the root.
class Executor {
public:
    virtual ~Executor() = default;

    void open() {
        auto start = std::chrono::steady_clock::now();
        open_impl();
        elapsed_ms_ += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    }

    std::optional<sql::storage::Row> next() {
        auto start = std::chrono::steady_clock::now();
        auto result = next_impl();
        elapsed_ms_ += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        if (result.has_value()) ++rows_produced_;
        return result;
    }

    void close() { close_impl(); } // releasing resources, not query work -- not timed

    virtual const RowSchema& schema() const = 0;
    virtual std::string operator_name() const = 0;
    // Children in the same order PhysicalPlan uses (input; or left, right)
    // -- lets a generic caller (the EXPLAIN ANALYZE printer) walk the
    // executor tree without knowing every concrete subclass.
    virtual std::vector<Executor*> children() const = 0;

    size_t rows_produced() const { return rows_produced_; }
    double elapsed_ms() const { return elapsed_ms_; }

protected:
    virtual void open_impl() = 0;
    virtual std::optional<sql::storage::Row> next_impl() = 0;
    virtual void close_impl() = 0;

private:
    size_t rows_produced_ = 0;
    double elapsed_ms_ = 0.0;
};

} // namespace sql::execution
