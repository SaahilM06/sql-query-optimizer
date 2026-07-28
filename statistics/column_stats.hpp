#pragma once

#include <optional>
#include <string>

#include "histogram.hpp"

namespace sql::statistics {

struct ColumnStats {
    double distinct_count = 0.0;  // NDV -- number of distinct non-null values
    double null_fraction = 0.0;   // fraction of rows where this column is NULL
    std::optional<double> min_value;
    std::optional<double> max_value;
    std::optional<Histogram> histogram;
};

} // namespace sql::statistics
