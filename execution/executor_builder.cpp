#include "executor_builder.hpp"

#include <stdexcept>

#include "operators.hpp"

namespace sql::execution {

using namespace sql::physical;

std::unique_ptr<Executor> build_executor(const PhysicalPlan& plan, const sql::storage::Database& db,
                                          const ExternalRowSets& external_rows) {
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
        case PhysicalPlan::Kind::ExternalRows: {
            auto it = external_rows.find(plan.count);
            if (it == external_rows.end()) {
                throw std::runtime_error("execution: no external rows supplied for slot " + std::to_string(plan.count));
            }
            // Each entry is "table.column" or a bare "column" (see
            // RowSchema::qualified_name) -- preserves per-column
            // qualifiers, which matters e.g. for a GROUP BY column that
            // keeps its original table alias through a HashAggregate.
            RowSchema schema;
            for (const auto& col : plan.projected_columns) {
                auto dot = col.rfind('.');
                if (dot == std::string::npos) {
                    schema.add(std::nullopt, col);
                } else {
                    schema.add(col.substr(0, dot), col.substr(dot + 1));
                }
            }
            // If this node stands in for an already-computed aggregate's
            // output (see distributed/coordinator.cpp's finish()), register
            // each aggregate exactly like HashAggregateExec's own
            // constructor does, so a HAVING/ORDER BY that re-mentions the
            // aggregate directly still resolves against it.
            for (size_t i = 0; i < plan.aggregates.size(); ++i) {
                schema.register_aggregate(plan.aggregates[i].func, plan.aggregates[i].arg, plan.group_by.size() + i);
            }
            return std::make_unique<ExternalRowsExec>(plan.table_name, std::move(schema), it->second);
        }
        case PhysicalPlan::Kind::Filter:
            return std::make_unique<FilterExec>(plan.predicate, build_executor(*plan.input, db, external_rows));
        case PhysicalPlan::Kind::NestedLoopJoin:
        case PhysicalPlan::Kind::IndexNestedLoopJoin:
            return std::make_unique<NestedLoopJoinExec>(plan.condition, build_executor(*plan.left, db, external_rows),
                                                          build_executor(*plan.right, db, external_rows));
        case PhysicalPlan::Kind::HashJoin:
            return std::make_unique<HashJoinExec>(plan.condition, build_executor(*plan.left, db, external_rows),
                                                    build_executor(*plan.right, db, external_rows));
        case PhysicalPlan::Kind::HashAggregate:
            return std::make_unique<HashAggregateExec>(plan.group_by, plan.aggregates,
                                                         build_executor(*plan.input, db, external_rows));
        case PhysicalPlan::Kind::Project:
            return std::make_unique<ProjectExec>(plan.expressions, build_executor(*plan.input, db, external_rows));
        case PhysicalPlan::Kind::Sort:
            return std::make_unique<SortExec>(plan.order_by, build_executor(*plan.input, db, external_rows));
        case PhysicalPlan::Kind::Limit:
            return std::make_unique<LimitExec>(plan.count, build_executor(*plan.input, db, external_rows));
    }
    throw std::runtime_error("execution: unknown PhysicalPlan::Kind");
}

} // namespace sql::execution
