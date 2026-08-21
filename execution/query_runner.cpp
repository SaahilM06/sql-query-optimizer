#include "query_runner.hpp"

#include <iomanip>

namespace sql::execution {

ExecutionResult run_to_completion(Executor& root) {
    ExecutionResult result;
    root.open();
    for (;;) {
        auto row = root.next();
        if (!row.has_value()) break;
        result.rows.push_back(*row);
    }
    root.close();
    result.total_elapsed_ms = root.elapsed_ms();
    return result;
}

namespace {

void print_node(const sql::physical::PhysicalPlan& plan, Executor& executor, std::ostream& os, int depth) {
    std::string indent(static_cast<size_t>(depth) * 2, ' ');
    os << indent << executor.operator_name() << "\n";
    os << indent << "  rows:  estimated " << plan.estimated_rows << "   actual " << executor.rows_produced() << "\n";
    os << indent << "  cost:  estimated " << plan.estimated_cost.total() << "   actual " << std::fixed
       << std::setprecision(3) << executor.elapsed_ms() << " ms" << std::defaultfloat << "\n";

    // PhysicalPlan and Executor are isomorphic by construction
    // (build_executor mirrors plan's shape node-for-node), so the Nth
    // entry in executor.children() is the executor for the Nth non-null
    // child slot here, in the same input/left/right order.
    std::vector<const sql::physical::PhysicalPlan*> plan_children;
    if (plan.input) plan_children.push_back(plan.input.get());
    if (plan.left) plan_children.push_back(plan.left.get());
    if (plan.right) plan_children.push_back(plan.right.get());

    auto children = executor.children();
    for (size_t i = 0; i < children.size() && i < plan_children.size(); ++i) {
        print_node(*plan_children[i], *children[i], os, depth + 1);
    }
}

} // namespace

void explain_analyze(const sql::physical::PhysicalPlan& plan, Executor& executor, std::ostream& os) {
    ExecutionResult result = run_to_completion(executor);
    os << "Executed: " << result.rows.size() << " rows in " << std::fixed << std::setprecision(3)
       << result.total_elapsed_ms << " ms" << std::defaultfloat << "\n\n";
    print_node(plan, executor, os, 0);
}

} // namespace sql::execution
