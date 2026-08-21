#pragma once

#include <cstddef>
#include <string>

#include "../logical/schema.hpp"
#include "../storage/database.hpp"

namespace sql::distributed {

// Static, primary-key-based partitioning: worker `worker_id` (of
// `num_workers`) owns rows where `id % num_workers == worker_id`, using
// whichever table's "id" column every table in this project's schema
// already has (see logical/schema.cpp). This deliberately does NOT
// guarantee co-partitioning on an arbitrary join's foreign key -- e.g.
// orders is partitioned by orders.id, not orders.customer_id, so an
// orders<->customers join needs a real exchange (broadcast or shuffle) to
// be correct, exactly like a real system's static base-table partitioning
// would. See distributed/coordinator.cpp for how that's handled.
size_t partition_of(int64_t id, size_t num_workers);

// Loads "<dir>/<table>.csv" for every table in schema_catalog, keeping only
// the rows this worker owns per partition_of() on that table's "id"
// column. A table with no "id" column loads in full on every worker
// (documented fallback, not silently wrong) -- true of nothing in this
// project's current schema, but a worker shouldn't refuse to start over a
// schema it doesn't recognize the convention for.
sql::storage::Database load_worker_partition(const std::string& data_dir, const sql::logical::Catalog& schema_catalog,
                                               size_t worker_id, size_t num_workers);

// Same partitioning rule as load_worker_partition, applied to an
// already-loaded Database instead of reading CSVs fresh -- what the
// coordinator's fault-tolerance path uses to recompute a dead worker's
// contribution itself from local_full_database, guaranteed to match
// exactly what that worker would have owned (same partition_of() math),
// without re-reading from disk. See distributed/coordinator.cpp's
// call_worker_or_recover.
sql::storage::Database filter_database_to_partition(const sql::storage::Database& full,
                                                      const sql::logical::Catalog& schema_catalog, size_t worker_id,
                                                      size_t num_workers);

} // namespace sql::distributed
