#pragma once

#include <cstdint>
#include <string>

#include "../logical/schema.hpp"
#include "../parser/ast.hpp"

namespace sql::integration {

// The two version segments in a plan cache key -- see build_cache_key. Meant
// to be computed once per process (schema/stats don't change per query, only
// on restart with different fixtures), not recomputed on every lookup.
struct CacheVersions {
    uint64_t schema_version = 0;
    uint64_t stats_version = 0;
};

// Hashes a canonical dump of every registered table (name, columns with
// type/nullability, row/byte stats, indexed columns) -- changes whenever the
// schema catalog's shape changes.
uint64_t compute_schema_version(const sql::logical::Catalog& catalog);

// Computes both version segments: schema_version from `catalog`,
// stats_version from every "*.json" file in `stats_dir` (see
// sql::statistics::fingerprint_stats_directory). Throws std::runtime_error
// if stats_dir can't be read.
CacheVersions compute_versions(const sql::logical::Catalog& catalog, const std::string& stats_dir);

// Builds the versioned plan cache key documented in cache/README.md:
// "plan:<query-hash>:<schema-version>:<stats-version>". The query hash is
// computed over the *canonicalized* SQL (see parser::to_canonical_sql), so
// equivalent queries that differ only in whitespace or keyword case hash to
// the same key.
std::string build_cache_key(const sql::parser::Statement& stmt, CacheVersions versions);

} // namespace sql::integration
