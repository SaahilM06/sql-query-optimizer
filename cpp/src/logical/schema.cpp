#include "schema.hpp"

namespace sql::logical {

Catalog Catalog::with_test_tables() {
    Catalog c;

    c.register_table("customers", TableSchema{
        {
            ColumnDef{"id", DataType::Int, false},
            ColumnDef{"name", DataType::Text, false},
            ColumnDef{"email", DataType::Text, true},
            ColumnDef{"country", DataType::Text, true},
        },
        TableStats{10'000, 128},
    });

    c.register_table("orders", TableSchema{
        {
            ColumnDef{"id", DataType::Int, false},
            ColumnDef{"customer_id", DataType::Int, false},
            ColumnDef{"total", DataType::Float, false},
            ColumnDef{"status", DataType::Text, true},
        },
        TableStats{500'000, 64},
    });

    c.register_table("products", TableSchema{
        {
            ColumnDef{"id", DataType::Int, false},
            ColumnDef{"name", DataType::Text, false},
            ColumnDef{"price", DataType::Float, false},
        },
        TableStats{2'000, 96},
    });

    return c;
}

} // namespace sql::logical
