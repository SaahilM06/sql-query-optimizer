#include "join_graph.hpp"

#include <functional>
#include <stdexcept>
#include <unordered_set>

namespace sql::optimizer {

using namespace sql::parser;
using namespace sql::logical;

std::optional<RelationId> JoinGraph::find_relation(const std::string& alias_or_name) const {
    for (const auto& rel : relations) {
        if (rel.alias.value_or(rel.table_name) == alias_or_name) return rel.id;
    }
    return std::nullopt;
}

namespace {

bool all_inner(const LogicalPlan& node) {
    if (node.kind != LogicalPlan::Kind::Join) return true;
    return node.join_type == JoinType::Inner && all_inner(*node.left) && all_inner(*node.right);
}

const LogicalPlan* unwrap_filters(const LogicalPlan& node) {
    const LogicalPlan* cur = &node;
    while (cur->kind == LogicalPlan::Kind::Filter) cur = cur->input.get();
    return cur;
}

// Flatten `a AND b AND c` into [a, b, c], copying (not consuming) `expr`.
std::vector<Expression> split_and_copy(const Expression& expr) {
    if (expr.kind == Expression::Kind::BinaryOp && expr.binary_op == BinaryOperator::And) {
        auto left = split_and_copy(*expr.left);
        auto right = split_and_copy(*expr.right);
        left.insert(left.end(), std::make_move_iterator(right.begin()), std::make_move_iterator(right.end()));
        return left;
    }
    std::vector<Expression> v;
    v.push_back(expr);
    return v;
}

struct RawExtraction {
    std::vector<Relation> relations;
    std::vector<Expression> predicates; // flat AND-conjuncts from every Filter and Join condition
};

void collect(const LogicalPlan& node, RawExtraction& out) {
    switch (node.kind) {
        case LogicalPlan::Kind::Scan: {
            RelationId id = out.relations.size();
            out.relations.push_back(Relation{id, node.table_name, node.alias, {}});
            break;
        }
        case LogicalPlan::Kind::Filter: {
            for (auto& conj : split_and_copy(node.predicate)) out.predicates.push_back(std::move(conj));
            collect(*node.input, out);
            break;
        }
        case LogicalPlan::Kind::Join: {
            for (auto& conj : split_and_copy(node.condition)) out.predicates.push_back(std::move(conj));
            collect(*node.left, out);
            collect(*node.right, out);
            break;
        }
        default:
            throw std::logic_error("join_graph: unexpected node kind inside a join-search subtree");
    }
}

// Which relation IDs does `expr` reference? nullopt if any Column can't be
// resolved (unqualified in this multi-relation context, or an alias not
// present in `graph`) -- callers treat that as "can't classify."
std::optional<std::unordered_set<RelationId>> referenced_relation_ids(const Expression& expr, const JoinGraph& graph) {
    std::unordered_set<RelationId> out;
    bool ok = true;

    std::function<void(const Expression&)> visit = [&](const Expression& e) {
        if (!ok) return;
        switch (e.kind) {
            case Expression::Kind::Column: {
                if (!e.table.has_value()) {
                    ok = false;
                    return;
                }
                auto id = graph.find_relation(*e.table);
                if (!id.has_value()) {
                    ok = false;
                    return;
                }
                out.insert(*id);
                break;
            }
            case Expression::Kind::BinaryOp:
                visit(*e.left);
                visit(*e.right);
                break;
            case Expression::Kind::UnaryOp:
                visit(*e.operand);
                break;
            case Expression::Kind::Function:
                for (const auto& a : e.args) visit(a);
                break;
            case Expression::Kind::Literal:
            case Expression::Kind::Wildcard:
                break;
        }
    };
    visit(expr);

    if (!ok) return std::nullopt;
    return out;
}

} // namespace

bool is_join_search_candidate(const LogicalPlan& node) {
    const LogicalPlan* core = unwrap_filters(node);
    return core->kind == LogicalPlan::Kind::Join && all_inner(*core);
}

JoinGraphExtraction build_join_graph(const LogicalPlan& root) {
    RawExtraction raw;
    collect(root, raw);

    JoinGraphExtraction result;
    result.graph.relations = std::move(raw.relations);

    for (auto& pred : raw.predicates) {
        auto refs = referenced_relation_ids(pred, result.graph);
        if (!refs.has_value()) {
            result.residual_filters.push_back(std::move(pred));
            continue;
        }

        if (refs->size() == 1) {
            RelationId id = *refs->begin();
            result.graph.relations[id].local_filters.push_back(std::move(pred));
        } else if (refs->size() == 2) {
            auto it = refs->begin();
            RelationId a = *it++;
            RelationId b = *it;
            result.graph.edges.push_back(JoinEdge{a, b, std::move(pred)});
        } else {
            // 0 relations (a constant predicate) or >2 relations -- can't
            // cleanly attribute to a single relation or a single edge.
            result.residual_filters.push_back(std::move(pred));
        }
    }

    return result;
}

} // namespace sql::optimizer
