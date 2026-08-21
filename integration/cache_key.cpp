#include "cache_key.hpp"

#include "../parser/sql_printer.hpp"
#include "../statistics/statistics_loader.hpp"
#include "../util/hash.hpp"

namespace sql::integration {

namespace {

const char* data_type_name(sql::logical::DataType t) {
    switch (t) {
        case sql::logical::DataType::Int: return "Int";
        case sql::logical::DataType::Float: return "Float";
        case sql::logical::DataType::Text: return "Text";
        case sql::logical::DataType::Boolean: return "Boolean";
    }
    return "?";
}

} // namespace

uint64_t compute_schema_version(const sql::logical::Catalog& catalog) {
    std::string dump;
    for (const auto& table_name : catalog.table_names()) {
        const auto* schema = catalog.get(table_name);
        if (schema == nullptr) continue; // table_names() is derived from the same map; defensive only.

        dump += table_name;
        dump += '\0';
        dump += std::to_string(schema->stats.row_count);
        dump += '\0';
        dump += std::to_string(schema->stats.avg_row_bytes);
        dump += '\0';

        for (const auto& col : schema->columns) {
            dump += col.name;
            dump += ':';
            dump += data_type_name(col.data_type);
            dump += ':';
            dump += (col.nullable ? '1' : '0');
            dump += '\0';
        }

        for (const auto& idx : schema->indexed_columns) {
            dump += idx;
            dump += '\0';
        }
    }
    return sql::util::fnv1a64(dump);
}

CacheVersions compute_versions(const sql::logical::Catalog& catalog, const std::string& stats_dir) {
    CacheVersions versions;
    versions.schema_version = compute_schema_version(catalog);
    versions.stats_version = sql::statistics::fingerprint_stats_directory(stats_dir);
    return versions;
}

std::string build_cache_key(const sql::parser::Statement& stmt, CacheVersions versions) {
    uint64_t query_hash = sql::util::fnv1a64(sql::parser::to_canonical_sql(stmt));
    return "plan:" + sql::util::hex64(query_hash) + ":" + std::to_string(versions.schema_version) + ":" +
           std::to_string(versions.stats_version);
}

} // namespace sql::integration
