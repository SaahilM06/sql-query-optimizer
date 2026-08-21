#include "plan_json.hpp"

#include <vector>

namespace sql::web {

using sql::util::JsonValue;

namespace {

const char* kind_name(sql::physical::PhysicalPlan::Kind k) {
    using Kind = sql::physical::PhysicalPlan::Kind;
    switch (k) {
        case Kind::SeqScan: return "SeqScan";
        case Kind::IndexScan: return "IndexScan";
        case Kind::Filter: return "Filter";
        case Kind::NestedLoopJoin: return "NestedLoopJoin";
        case Kind::HashJoin: return "HashJoin";
        case Kind::IndexNestedLoopJoin: return "IndexNestedLoopJoin";
        case Kind::HashAggregate: return "HashAggregate";
        case Kind::Project: return "Project";
        case Kind::Sort: return "Sort";
        case Kind::Limit: return "Limit";
        case Kind::ExternalRows: return "ExternalRows";
    }
    return "?";
}

} // namespace

JsonValue plan_to_json_tree(const sql::physical::PhysicalPlan& plan, sql::execution::Executor* executor) {
    JsonValue node = JsonValue::make_object();
    node.object_val["operator"] = JsonValue::make_string(kind_name(plan.kind));
    node.object_val["estimated_rows"] = JsonValue::make_number(static_cast<double>(plan.estimated_rows));
    node.object_val["estimated_cost"] = JsonValue::make_number(plan.estimated_cost.total());
    node.object_val["reasoning"] = JsonValue::make_string(plan.cardinality_reasoning);
    if (!plan.table_name.empty()) node.object_val["table"] = JsonValue::make_string(plan.table_name);

    if (executor != nullptr) {
        node.object_val["actual_rows"] = JsonValue::make_number(static_cast<double>(executor->rows_produced()));
        node.object_val["actual_time_ms"] = JsonValue::make_number(executor->elapsed_ms());
    }

    // PhysicalPlan and Executor are isomorphic by construction
    // (build_executor mirrors plan's shape node-for-node -- see
    // execution/executor_builder.cpp), so the Nth entry of
    // executor->children() is the executor for the Nth non-null child slot
    // here, in the same input/left/right order.
    std::vector<const sql::physical::PhysicalPlan*> plan_children;
    if (plan.input) plan_children.push_back(plan.input.get());
    if (plan.left) plan_children.push_back(plan.left.get());
    if (plan.right) plan_children.push_back(plan.right.get());

    std::vector<sql::execution::Executor*> exec_children;
    if (executor != nullptr) exec_children = executor->children();

    JsonValue children = JsonValue::make_array();
    for (size_t i = 0; i < plan_children.size(); ++i) {
        sql::execution::Executor* child_exec = (i < exec_children.size()) ? exec_children[i] : nullptr;
        children.array_val.push_back(plan_to_json_tree(*plan_children[i], child_exec));
    }
    node.object_val["children"] = std::move(children);

    return node;
}

} // namespace sql::web
