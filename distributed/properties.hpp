#pragma once

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

#include "../parser/ast.hpp"

namespace sql::distributed {

// What column (if any) a distributed (sub)plan's output rows are
// guaranteed to be co-located by across workers -- e.g. after a shuffle
// join on `o.customer_id = c.id`, every row for a given customer_id is
// guaranteed to be on exactly one worker, and no other worker holds any
// row for that same customer_id. That guarantee is what lets a later step
// skip work that's only needed when it *doesn't* hold -- see
// coordinator.cpp's shuffle+aggregate path, which skips the cross-worker
// merge entirely when the GROUP BY is on this same key, since each
// worker's partial aggregate is then already the final, complete answer
// for every group it produced. This is deliberately narrow: it tracks one
// property (a single equi-partition column) for one place it's used, not a
// general partitioning/ordering system threaded through the whole
// optimizer -- see ROADMAP.md for why that fuller version (letting a 3+
// table join reason about per-subtree partitioning, not just the
// outermost join) is future work.
struct PartitionKey {
    std::optional<std::string> table;
    std::string column;

    bool matches(const sql::parser::Expression& e) const {
        return e.kind == sql::parser::Expression::Kind::Column && e.table == table && e.column == column;
    }
};

// True if `key` is guaranteed to distinguish every group in `group_by` --
// i.e. two rows agreeing on `key` are guaranteed to agree on which group
// they belong to, because `key` is one of the columns being grouped on.
// Sufficient for the merge-skip optimization even when GROUP BY has other
// columns too: co-location on `key` alone still guarantees co-location of
// the full group tuple.
inline bool partition_key_is_safe_for_group_by(const PartitionKey& key, const std::vector<sql::parser::Expression>& group_by) {
    return std::any_of(group_by.begin(), group_by.end(), [&](const sql::parser::Expression& e) { return key.matches(e); });
}

} // namespace sql::distributed
