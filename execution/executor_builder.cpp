#include "executor_builder.hpp"

#include <stdexcept>

#include "operators.hpp"

namespace sql::execution {

using namespace sql::physical;

std::unique_ptr<Executor> build_executor(const PhysicalPlan& plan, const sql::storage::Database& db) {
    switch (plan.kind) {
        case PhysicalPlan::Kind::SeqScan: {
            const auto* table = db.get(plan.table_name);
            if (table == nullptr) throw std::runtime_error("execution: no data loaded for table '" + plan.table_name + "'");
            return std::make_unique<SeqScanExec>(plan.table_name, plan.alias, *table);
        }
        case PhysicalPlan::Kind::IndexScan: {
            const auto* table = db.get(plan.table_name);
            if (table == nullptr) throw std::runtime_error("execution: no data loaded for table '" + plan.table_name + "'");
            return std::make_unique<IndexScanExec>(plan.table_name, plan.alias, *table, plan.index_column,
                                                     plan.index_probe_value);
        }
        case PhysicalPlan::Kind::Filter:
            return std::make_unique<FilterExec>(plan.predicate, build_executor(*plan.input, db));
        case PhysicalPlan::Kind::NestedLoopJoin:
        case PhysicalPlan::Kind::IndexNestedLoopJoin:
            return std::make_unique<NestedLoopJoinExec>(plan.condition, build_executor(*plan.left, db),
                                                          build_executor(*plan.right, db));
        case PhysicalPlan::Kind::HashJoin:
            return std::make_unique<HashJoinExec>(plan.condition, build_executor(*plan.left, db),
                                                    build_executor(*plan.right, db));
        case PhysicalPlan::Kind::HashAggregate:
            return std::make_unique<HashAggregateExec>(plan.group_by, plan.aggregates, build_executor(*plan.input, db));
        case PhysicalPlan::Kind::Project:
            return std::make_unique<ProjectExec>(plan.expressions, build_executor(*plan.input, db));
        case PhysicalPlan::Kind::Sort:
            return std::make_unique<SortExec>(plan.order_by, build_executor(*plan.input, db));
        case PhysicalPlan::Kind::Limit:
            return std::make_unique<LimitExec>(plan.count, build_executor(*plan.input, db));
    }
    throw std::runtime_error("execution: unknown PhysicalPlan::Kind");
}

} // namespace sql::execution
