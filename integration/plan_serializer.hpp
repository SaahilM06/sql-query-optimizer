#pragma once

#include <string>

#include "../physical/physical_plan.hpp"

namespace sql::integration {

// Serializes a PhysicalPlan (the full tree, recursively) to a JSON string
// suitable for storing as a cache value. Every field of every node is
// written, including ones that are unused for a given node's Kind (e.g.
// `left`/`right` on a SeqScan are just absent/null) -- simpler and more
// robust than a per-Kind schema, at the cost of some redundant bytes.
std::string serialize_plan(const sql::physical::PhysicalPlan& plan);

// Inverse of serialize_plan. Throws std::runtime_error on malformed JSON or
// an unrecognized enum tag (e.g. a value written by a future, incompatible
// version of this serializer) -- callers should treat that as a cache miss,
// not a crash.
sql::physical::PhysicalPlan deserialize_plan(const std::string& json);

} // namespace sql::integration
