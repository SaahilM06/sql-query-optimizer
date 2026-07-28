#pragma once

#include <optional>
#include <string>
#include <vector>

#include "../logical/logical_plan.hpp"
#include "../parser/ast.hpp"

namespace sql::optimizer {

using RelationId = size_t;

// One base table participating in a join search, plus every predicate
// that references only this table (already isolated so it can be pushed
// into the base access path instead of applied after some join).
struct Relation {
    RelationId id;
    std::string table_name;
    std::optional<std::string> alias;
    std::vector<sql::parser::Expression> local_filters;
};

// A predicate connecting exactly two relations -- either a join's own ON
// condition, or a WHERE-clause predicate that happens to reference exactly
// two tables.
struct JoinEdge {
    RelationId left;
    RelationId right;
    sql::parser::Expression predicate;
};

struct JoinGraph {
    std::vector<Relation> relations;
    std::vector<JoinEdge> edges;

    // Finds a relation by alias (or bare table name if it has none).
    std::optional<RelationId> find_relation(const std::string& alias_or_name) const;
};

// True if `node`, unwrapped through any number of Filter wrappers, is an
// all-INNER-join subtree -- i.e. something build_join_graph can turn into
// a JoinGraph for the DP search. A bare Scan (nothing to join), an outer
// join anywhere in the subtree (order semantics -- out of scope for the
// join-order search), or any other node kind returns false.
bool is_join_search_candidate(const sql::logical::LogicalPlan& node);

// build_join_graph's result: the graph itself, plus any predicate that
// couldn't be cleanly attributed to exactly one or two relations (an
// unqualified column in a multi-relation context, a predicate spanning
// more than two tables, or a constant predicate with no columns at all).
// Callers wrap the final chosen plan in an extra Filter for each residual.
struct JoinGraphExtraction {
    JoinGraph graph;
    std::vector<sql::parser::Expression> residual_filters;
};

// Extracts a join graph from a LogicalPlan subtree for which
// is_join_search_candidate returns true. Every Filter predicate and every
// Join's condition in the subtree is split into AND-conjuncts and
// reclassified from scratch by which relations it actually references --
// this recovers correct per-relation attribution even where Part 1's
// predicate pushdown only pushed a filter down to wrap a multi-relation
// join subtree rather than the specific base Scan it belongs to.
JoinGraphExtraction build_join_graph(const sql::logical::LogicalPlan& root);

} // namespace sql::optimizer
