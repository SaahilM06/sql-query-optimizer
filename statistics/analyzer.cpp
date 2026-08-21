#include "analyzer.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_set>

#include "../parser/ast.hpp"

namespace sql::statistics {

namespace {

constexpr size_t kHistogramBuckets = 10;
// Not physical/cost.hpp's kPageSizeBytes -- statistics/ deliberately doesn't
// depend on physical/ (physical already depends on statistics/, and this
// avoids a cycle). Same value, kept as its own local constant.
constexpr double kAssumedPageBytes = 8192.0;

using sql::parser::Literal;

// Just needs to be a consistent, distinguishing key for a hash set --
// doesn't need to be pretty or match execution::literal_to_string's display
// formatting.
std::string literal_set_key(const Literal& v) {
    switch (v.kind) {
        case Literal::Kind::Integer: return "i:" + std::to_string(v.int_val);
        case Literal::Kind::Float: return "f:" + std::to_string(v.float_val);
        case Literal::Kind::Str: return "s:" + v.str_val;
        case Literal::Kind::Boolean: return v.bool_val ? "b:1" : "b:0";
        case Literal::Kind::Null: return "n:";
    }
    return "";
}

bool is_numeric(sql::logical::DataType t) {
    return t == sql::logical::DataType::Int || t == sql::logical::DataType::Float;
}

double as_double(const Literal& v) {
    if (v.kind == Literal::Kind::Integer) return static_cast<double>(v.int_val);
    if (v.kind == Literal::Kind::Float) return v.float_val;
    return 0.0;
}

double approx_literal_bytes(const Literal& v) {
    if (v.kind == Literal::Kind::Str) return static_cast<double>(v.str_val.size());
    if (v.kind == Literal::Kind::Null) return 0.0;
    return 8.0; // Int/Float/Boolean -- fixed-width enough that a flat estimate is fine
}

} // namespace

TableStats analyze_table(const sql::storage::Table& table, const sql::logical::TableSchema& schema) {
    TableStats stats;
    stats.row_count = static_cast<double>(table.row_count());

    const auto& column_names = table.column_names();
    const auto& rows = table.rows();
    double total_bytes = 0.0;

    for (size_t c = 0; c < column_names.size(); ++c) {
        const std::string& name = column_names[c];
        const auto* col_def = schema.get_column(name);
        if (col_def == nullptr) continue; // load_table_from_csv already guarantees this can't happen

        ColumnStats cs;
        std::unordered_set<std::string> distinct_values;
        size_t null_count = 0;
        double min_v = 0.0, max_v = 0.0;
        bool have_range = false;

        for (const auto& row : rows) {
            const Literal& v = row[c];
            total_bytes += approx_literal_bytes(v);
            if (v.kind == Literal::Kind::Null) {
                ++null_count;
                continue;
            }
            distinct_values.insert(literal_set_key(v));
            if (is_numeric(col_def->data_type)) {
                double d = as_double(v);
                if (!have_range) {
                    min_v = d;
                    max_v = d;
                    have_range = true;
                } else {
                    min_v = std::min(min_v, d);
                    max_v = std::max(max_v, d);
                }
            }
        }

        cs.distinct_count = static_cast<double>(distinct_values.size());
        cs.null_fraction = rows.empty() ? 0.0 : static_cast<double>(null_count) / static_cast<double>(rows.size());

        size_t non_null = rows.size() - null_count;
        if (is_numeric(col_def->data_type) && have_range && non_null > 0) {
            cs.min_value = min_v;
            cs.max_value = max_v;

            Histogram hist;
            if (max_v <= min_v) {
                // Every non-null value is identical -- one degenerate
                // bucket, matching how Histogram::range_selectivity already
                // special-cases a zero-width bucket.
                hist.buckets.push_back(HistogramBucket{min_v, max_v, 1.0});
            } else {
                std::vector<size_t> counts(kHistogramBuckets, 0);
                double width = (max_v - min_v) / static_cast<double>(kHistogramBuckets);
                for (const auto& row : rows) {
                    const Literal& v = row[c];
                    if (v.kind == Literal::Kind::Null) continue;
                    size_t bucket = static_cast<size_t>((as_double(v) - min_v) / width);
                    if (bucket >= kHistogramBuckets) bucket = kHistogramBuckets - 1; // as_double(v) == max_v lands here
                    ++counts[bucket];
                }
                for (size_t b = 0; b < kHistogramBuckets; ++b) {
                    double lower = min_v + width * static_cast<double>(b);
                    double upper = (b + 1 == kHistogramBuckets) ? max_v : min_v + width * static_cast<double>(b + 1);
                    double frequency = static_cast<double>(counts[b]) / static_cast<double>(non_null);
                    hist.buckets.push_back(HistogramBucket{lower, upper, frequency});
                }
            }
            cs.histogram = std::move(hist);
        }

        stats.columns[name] = std::move(cs);
    }

    double avg_row_bytes = rows.empty() ? 0.0 : total_bytes / static_cast<double>(rows.size());
    stats.page_count = std::ceil(static_cast<double>(table.row_count()) * avg_row_bytes / kAssumedPageBytes);

    return stats;
}

StatisticsCatalog analyze_database(const sql::storage::Database& db, const sql::logical::Catalog& schema_catalog) {
    StatisticsCatalog catalog;
    for (const auto& name : schema_catalog.table_names()) {
        const auto* table = db.get(name);
        const auto* schema = schema_catalog.get(name);
        if (table == nullptr || schema == nullptr) continue;
        catalog.register_table(name, analyze_table(*table, *schema));
    }
    return catalog;
}

} // namespace sql::statistics
