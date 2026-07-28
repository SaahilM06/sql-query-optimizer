#include "explain.hpp"

#include <string>

namespace sql::optimizer {

using namespace sql::physical;

namespace {

const char* kind_name(PhysicalPlan::Kind k) {
    switch (k) {
        case PhysicalPlan::Kind::SeqScan: return "SeqScan";
        case PhysicalPlan::Kind::IndexScan: return "IndexScan";
        case PhysicalPlan::Kind::Filter: return "Filter";
        case PhysicalPlan::Kind::NestedLoopJoin: return "NestedLoopJoin";
        case PhysicalPlan::Kind::HashJoin: return "HashJoin";
        case PhysicalPlan::Kind::HashAggregate: return "HashAggregate";
        case PhysicalPlan::Kind::Project: return "Project";
        case PhysicalPlan::Kind::Sort: return "Sort";
        case PhysicalPlan::Kind::Limit: return "Limit";
    }
    return "?";
}

std::string indent(int depth) { return std::string(static_cast<size_t>(depth) * 2, ' '); }

} // namespace

void explain_plan(const PhysicalPlan& plan, std::ostream& os, int depth) {
    std::string pad = indent(depth);

    os << pad << kind_name(plan.kind);
    if (plan.kind == PhysicalPlan::Kind::SeqScan || plan.kind == PhysicalPlan::Kind::IndexScan) {
        os << "(" << plan.table_name << ")";
    }
    os << "\n";

    os << pad << "  estimated rows: " << plan.estimated_rows << "\n";
    os << pad << "  cost: cpu=" << plan.estimated_cost.cpu << " io=" << plan.estimated_cost.io
       << " memory=" << plan.estimated_cost.memory << " total=" << plan.estimated_cost.total() << "\n";

    if (!plan.cardinality_reasoning.empty()) {
        os << pad << "  source: " << plan.cardinality_reasoning << " (confidence " << plan.cardinality_confidence
           << ")\n";
    }

    switch (plan.kind) {
        case PhysicalPlan::Kind::Filter:
        case PhysicalPlan::Kind::HashAggregate:
        case PhysicalPlan::Kind::Project:
        case PhysicalPlan::Kind::Sort:
        case PhysicalPlan::Kind::Limit:
            explain_plan(*plan.input, os, depth + 1);
            break;
        case PhysicalPlan::Kind::NestedLoopJoin:
        case PhysicalPlan::Kind::HashJoin:
            explain_plan(*plan.left, os, depth + 1);
            explain_plan(*plan.right, os, depth + 1);
            break;
        default:
            break; // SeqScan / IndexScan are leaves
    }
}

} // namespace sql::optimizer
