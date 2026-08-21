#pragma once

#include <string>
#include <vector>

namespace sql::storage {

// Reads a CSV file into raw string fields -- one vector<string> per row,
// including the header as row 0. Handles double-quoted fields with embedded
// commas/newlines and "" as an escaped quote (basic RFC4180). Throws
// std::runtime_error if the file can't be opened.
std::vector<std::vector<std::string>> read_csv(const std::string& path);

} // namespace sql::storage
