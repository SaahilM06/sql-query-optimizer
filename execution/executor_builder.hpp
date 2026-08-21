#pragma once

#include <memory>

#include "../physical/physical_plan.hpp"
#include "../storage/database.hpp"
#include "executor.hpp"

namespace sql::execution {

// Recursively builds an executor tree isomorphic to `plan`'s shape --
// exactly one Executor per PhysicalPlan node, same input/left/right
// structure -- so the two trees can later be walked in lockstep (see
// explain_analyze in query_runner.hpp). Throws std::runtime_error if `plan`
// references a table with no data loaded into `db` (see
// storage::load_database_from_directory).
std::unique_ptr<Executor> build_executor(const sql::physical::PhysicalPlan& plan, const sql::storage::Database& db);

} // namespace sql::execution
