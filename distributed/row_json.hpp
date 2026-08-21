#pragma once

#include "../storage/row.hpp"
#include "../util/json.hpp"

namespace sql::distributed {

// Serializes/deserializes a row set for shipping over the worker RPC
// protocol. Independent of integration/plan_serializer.cpp's own
// Literal<->JSON encoding (that one is private to that file) -- small,
// deliberate duplication rather than exposing those internals or coupling
// two otherwise-unrelated serialization concerns.
sql::util::JsonValue rows_to_json(const std::vector<sql::storage::Row>& rows);
std::vector<sql::storage::Row> rows_from_json(const sql::util::JsonValue& value);

} // namespace sql::distributed
