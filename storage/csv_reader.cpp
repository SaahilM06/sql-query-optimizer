#include "csv_reader.hpp"

#include <fstream>
#include <sstream>
#include <stdexcept>

namespace sql::storage {

std::vector<std::vector<std::string>> read_csv(const std::string& path) {
    std::ifstream file(path);
    if (!file) throw std::runtime_error("csv: could not open file: " + path);
    std::stringstream buffer;
    buffer << file.rdbuf();
    const std::string& text = buffer.str();

    std::vector<std::vector<std::string>> rows;
    std::vector<std::string> current_row;
    std::string field;
    bool in_quotes = false;

    auto end_field = [&]() {
        current_row.push_back(field);
        field.clear();
    };
    auto end_row = [&]() {
        end_field();
        rows.push_back(current_row);
        current_row.clear();
    };

    size_t i = 0;
    while (i < text.size()) {
        char c = text[i];
        if (in_quotes) {
            if (c == '"') {
                if (i + 1 < text.size() && text[i + 1] == '"') {
                    field.push_back('"');
                    i += 2;
                    continue;
                }
                in_quotes = false;
                ++i;
                continue;
            }
            field.push_back(c);
            ++i;
            continue;
        }
        if (c == '"') {
            in_quotes = true;
            ++i;
            continue;
        }
        if (c == ',') {
            end_field();
            ++i;
            continue;
        }
        if (c == '\r') {
            ++i;
            continue;
        }
        if (c == '\n') {
            end_row();
            ++i;
            continue;
        }
        field.push_back(c);
        ++i;
    }
    // Last line with no trailing newline.
    if (!field.empty() || !current_row.empty()) end_row();

    return rows;
}

} // namespace sql::storage
