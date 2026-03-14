use std::collections::HashMap;

// ── Column ───────────────────────────────────────────────────────────────────

#[derive(Debug, Clone, PartialEq)]
pub enum DataType {
    Int,
    Float,
    Text,
    Boolean,
}

#[derive(Debug, Clone)]
pub struct ColumnDef {
    pub name: String,
    pub data_type: DataType,
    pub nullable: bool,
}

// ── Table ────────────────────────────────────────────────────────────────────

/// Row count + average row size in bytes.
/// The costing layer uses these to estimate join output sizes and scan costs.
#[derive(Debug, Clone)]
pub struct TableStats {
    pub row_count: usize,
    pub avg_row_bytes: usize,
}

#[derive(Debug, Clone)]
pub struct TableSchema {
    pub columns: Vec<ColumnDef>,
    pub stats: TableStats,
}

impl TableSchema {
    pub fn get_column(&self, name: &str) -> Option<&ColumnDef> {
        self.columns.iter().find(|c| c.name == name)
    }
}

// ── Catalog ──────────────────────────────────────────────────────────────────

#[derive(Debug, Default)]
pub struct Catalog {
    tables: HashMap<String, TableSchema>,
}

impl Catalog {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn register(&mut self, name: &str, schema: TableSchema) {
        self.tables.insert(name.to_string(), schema);
    }

    pub fn get(&self, name: &str) -> Option<&TableSchema> {
        self.tables.get(name)
    }

    /// Pre-populated catalog for development and testing.
    pub fn with_test_tables() -> Self {
        let mut c = Self::new();

        c.register("customers", TableSchema {
            columns: vec![
                ColumnDef { name: "id".into(),      data_type: DataType::Int,     nullable: false },
                ColumnDef { name: "name".into(),     data_type: DataType::Text,    nullable: false },
                ColumnDef { name: "email".into(),    data_type: DataType::Text,    nullable: true  },
                ColumnDef { name: "country".into(),  data_type: DataType::Text,    nullable: true  },
            ],
            stats: TableStats { row_count: 10_000, avg_row_bytes: 128 },
        });

        c.register("orders", TableSchema {
            columns: vec![
                ColumnDef { name: "id".into(),           data_type: DataType::Int,   nullable: false },
                ColumnDef { name: "customer_id".into(),  data_type: DataType::Int,   nullable: false },
                ColumnDef { name: "total".into(),        data_type: DataType::Float, nullable: false },
                ColumnDef { name: "status".into(),       data_type: DataType::Text,  nullable: true  },
            ],
            stats: TableStats { row_count: 500_000, avg_row_bytes: 64 },
        });

        c.register("products", TableSchema {
            columns: vec![
                ColumnDef { name: "id".into(),     data_type: DataType::Int,   nullable: false },
                ColumnDef { name: "name".into(),   data_type: DataType::Text,  nullable: false },
                ColumnDef { name: "price".into(),  data_type: DataType::Float, nullable: false },
            ],
            stats: TableStats { row_count: 2_000, avg_row_bytes: 96 },
        });

        c
    }
}
