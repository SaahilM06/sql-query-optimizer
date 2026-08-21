#!/usr/bin/env python3
"""Generates the small, referentially-consistent CSV dataset in data/ that
the real execution engine (execution/, storage/) runs queries against.

This is intentionally much smaller than the stats/*.json fixtures (which
describe production-scale statistics for cost-model testing) -- it just
needs to be big enough to produce interesting multi-row results and let
EXPLAIN ANALYZE show a real estimated-vs-actual comparison. Schema matches
logical::Catalog::with_test_tables() in logical/schema.cpp exactly; if that
schema changes, this script needs to change with it.

Deterministic: fixed random seed, so re-running reproduces the same data
(useful for tests/benchmarks that want stable row counts).
"""
import csv
import random

random.seed(42)

COUNTRIES = ["US", "US", "US", "CA", "UK", "DE", "FR", "IN", "BR", "AU"]
CATEGORIES = ["electronics", "electronics", "electronics", "home", "clothing", "toys", "books"]
STATUSES = ["completed", "completed", "completed", "pending", "cancelled"]

NUM_CUSTOMERS = 300
NUM_PRODUCTS = 50
NUM_ORDERS = 3000


def write_csv(path, header, rows):
    with open(path, "w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(header)
        writer.writerows(rows)


def main():
    customers = []
    for i in range(1, NUM_CUSTOMERS + 1):
        customers.append([i, f"Customer {i}", f"customer{i}@example.com", random.choice(COUNTRIES)])
    write_csv("data/customers.csv", ["id", "name", "email", "country"], customers)

    products = []
    for i in range(1, NUM_PRODUCTS + 1):
        products.append([i, f"Product {i}", round(random.uniform(5, 500), 2), random.choice(CATEGORIES)])
    write_csv("data/products.csv", ["id", "name", "price", "category"], products)

    orders = []
    for i in range(1, NUM_ORDERS + 1):
        orders.append([
            i,
            random.randint(1, NUM_CUSTOMERS),
            random.randint(1, NUM_PRODUCTS),
            round(random.uniform(5, 1000), 2),
            random.choice(STATUSES),
        ])
    write_csv("data/orders.csv", ["id", "customer_id", "product_id", "total", "status"], orders)

    print(f"Wrote {NUM_CUSTOMERS} customers, {NUM_PRODUCTS} products, {NUM_ORDERS} orders to data/")


if __name__ == "__main__":
    main()
