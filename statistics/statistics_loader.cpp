#include "statistics_loader.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <vector>

#include "../util/hash.hpp"
#include "../util/json.hpp"

namespace sql::statistics {

namespace {

using sql::util::JsonValue;

TableStats table_stats_from_json(const JsonValue& root) {
    TableStats stats;
    if (const auto* rc = root.find("row_count")) stats.row_count = rc->as_number();
    if (const auto* pc = root.find("page_count")) stats.page_count = pc->as_number();

    const auto* cols = root.find("columns");
    if (cols == nullptr || cols->kind != JsonValue::Kind::Object) return stats;

    for (const auto& [name, col_val] : cols->object_val) {
        ColumnStats cs;
        if (const auto* dc = col_val.find("distinct_count")) cs.distinct_count = dc->as_number();
        if (const auto* nf = col_val.find("null_fraction")) cs.null_fraction = nf->as_number();
        if (const auto* mn = col_val.find("min")) cs.min_value = mn->as_number();
        if (const auto* mx = col_val.find("max")) cs.max_value = mx->as_number();

        if (const auto* hist = col_val.find("histogram")) {
            if (const auto* buckets = hist->find("buckets"); buckets != nullptr && buckets->kind == JsonValue::Kind::Array) {
                Histogram h;
                for (const auto& b : buckets->array_val) {
                    HistogramBucket bucket{};
                    if (const auto* lo = b.find("lower")) bucket.lower = lo->as_number();
                    if (const auto* hi = b.find("upper")) bucket.upper = hi->as_number();
                    if (const auto* fr = b.find("frequency")) bucket.frequency = fr->as_number();
                    h.buckets.push_back(bucket);
                }
                cs.histogram = std::move(h);
            }
        }

        stats.columns[name] = std::move(cs);
    }

    return stats;
}

} // namespace

TableStats load_table_stats_from_file(const std::string& path) {
    std::ifstream file(path);
    if (!file) {
        throw std::runtime_error("statistics: could not open file: " + path);
    }
    std::stringstream buffer;
    buffer << file.rdbuf();

    JsonValue root = sql::util::parse_json(buffer.str());
    return table_stats_from_json(root);
}

StatisticsCatalog load_catalog_from_directory(const std::string& dir_path) {
    StatisticsCatalog catalog;

    if (!std::filesystem::exists(dir_path) || !std::filesystem::is_directory(dir_path)) {
        throw std::runtime_error("statistics: not a directory: " + dir_path);
    }

    for (const auto& entry : std::filesystem::directory_iterator(dir_path)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".json") continue;

        std::string table_name = entry.path().stem().string();
        TableStats stats = load_table_stats_from_file(entry.path().string());
        catalog.register_table(table_name, std::move(stats));
    }

    return catalog;
}

uint64_t fingerprint_stats_directory(const std::string& dir_path) {
    if (!std::filesystem::exists(dir_path) || !std::filesystem::is_directory(dir_path)) {
        throw std::runtime_error("statistics: not a directory: " + dir_path);
    }

    std::vector<std::filesystem::path> paths;
    for (const auto& entry : std::filesystem::directory_iterator(dir_path)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".json") continue;
        paths.push_back(entry.path());
    }
    std::sort(paths.begin(), paths.end());

    // Concatenate filename + contents for every file, in sorted order, and
    // hash the result as one blob. Prefixing each file's bytes with its name
    // (rather than just concatenating contents) means adding/removing/
    // renaming a stats file changes the fingerprint even if two files
    // happen to have identical byte contents.
    std::string combined;
    for (const auto& path : paths) {
        std::ifstream file(path);
        if (!file) {
            throw std::runtime_error("statistics: could not open file: " + path.string());
        }
        std::stringstream buffer;
        buffer << file.rdbuf();

        combined += path.filename().string();
        combined += '\0';
        combined += buffer.str();
        combined += '\0';
    }

    return sql::util::fnv1a64(combined);
}

} // namespace sql::statistics
