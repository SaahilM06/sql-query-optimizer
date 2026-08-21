#pragma once

#include <memory>
#include <unordered_map>
#include <vector>

#include "../physical/physical_plan.hpp"
#include "../storage/database.hpp"
#include "executor.hpp"

namespace sql::execution {

// Rows to inject at a PhysicalPlan::Kind::ExternalRows leaf, keyed by its
// `count` field (used as a slot id). Empty by default -- a plan with no
// ExternalRows nodes (the normal, non-distributed case) never looks at
// this.
using ExternalRowSets = std::unordered_map<size_t, std::vector<sql::storage::Row>>;

// Recursively builds an executor tree isomorphic to `plan`'s shape --
// exactly one Executor per PhysicalPlan node, same input/left/right
// structure -- so the two trees can later be walked in lockstep (see
// explain_analyze in query_runner.hpp). Throws std::runtime_error if `plan`
// references a table with no data loaded into `db`, or an ExternalRows
// node whose slot isn't present in `external_rows`.
std::unique_ptr<Executor> build_executor(const sql::physical::PhysicalPlan& plan, const sql::storage::Database& db,
                                          const ExternalRowSets& external_rows = {});

} // namespace sql::execution
