#include "trident/sparql_ast.hpp"

#include <algorithm>

namespace trident {

const char* expr_op_name(ExprOp op) {
    switch (op) {
        case ExprOp::Or: return "||";
        case ExprOp::And: return "&&";
        case ExprOp::Equal: return "=";
        case ExprOp::NotEqual: return "!=";
        case ExprOp::Less: return "<";
        case ExprOp::Greater: return ">";
        case ExprOp::LessEqual: return "<=";
        case ExprOp::GreaterEqual: return ">=";
        case ExprOp::Add: return "+";
        case ExprOp::Subtract: return "-";
        case ExprOp::Multiply: return "*";
        case ExprOp::Divide: return "/";
        case ExprOp::Not: return "!";
        case ExprOp::Negate: return "-";
    }
    return "?";
}

const char* aggregate_name(AggregateFunc func) {
    switch (func) {
        case AggregateFunc::Count: return "COUNT";
        case AggregateFunc::Sum: return "SUM";
        case AggregateFunc::Min: return "MIN";
        case AggregateFunc::Max: return "MAX";
        case AggregateFunc::Avg: return "AVG";
        case AggregateFunc::SampleFirst: return "SAMPLE";
    }
    return "?";
}

ExprPtr make_expr(std::variant<VarExpr, ConstExpr, OpExpr, CallExpr> node) {
    auto out = std::make_unique<Expr>();
    out->node = std::move(node);
    return out;
}

std::string expr_to_string(const Expr& expr) {
    return std::visit(
        [](const auto& node) -> std::string {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, VarExpr>) {
                return "?" + node.name;
            } else if constexpr (std::is_same_v<T, ConstExpr>) {
                return node.term.to_display();
            } else if constexpr (std::is_same_v<T, OpExpr>) {
                if (!node.rhs) {
                    return std::string(expr_op_name(node.op)) + "(" + expr_to_string(*node.lhs) +
                           ")";
                }
                return "(" + expr_to_string(*node.lhs) + " " + expr_op_name(node.op) + " " +
                       expr_to_string(*node.rhs) + ")";
            } else {
                std::string out = node.name + "(";
                for (std::size_t i = 0; i < node.args.size(); ++i) {
                    if (i) out += ", ";
                    out += expr_to_string(*node.args[i]);
                }
                return out + ")";
            }
        },
        expr.node);
}

void collect_expr_variables(const Expr& expr, std::vector<std::string>& out) {
    auto push = [&out](const std::string& name) {
        if (std::find(out.begin(), out.end(), name) == out.end()) out.push_back(name);
    };
    std::visit(
        [&](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, VarExpr>) {
                push(node.name);
            } else if constexpr (std::is_same_v<T, OpExpr>) {
                if (node.lhs) collect_expr_variables(*node.lhs, out);
                if (node.rhs) collect_expr_variables(*node.rhs, out);
            } else if constexpr (std::is_same_v<T, CallExpr>) {
                for (const ExprPtr& arg : node.args) collect_expr_variables(*arg, out);
            }
        },
        expr.node);
}

namespace {

std::string pad(int indent) { return std::string(static_cast<std::size_t>(indent) * 2, ' '); }

std::string join_list(const std::vector<std::string>& items) {
    std::string out;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i) out += ", ";
        out += "?" + items[i];
    }
    return out;
}

}  // namespace

std::string algebra_to_string(const Algebra& algebra, int indent) {
    const std::string p = pad(indent);
    return std::visit(
        [&](const auto& node) -> std::string {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, BgpNode>) {
                std::string out = p + "BGP\n";
                for (const TriplePattern& tp : node.patterns) {
                    out += pad(indent + 1) + tp.to_string() + "\n";
                }
                return out;
            } else if constexpr (std::is_same_v<T, JoinNode>) {
                return p + "Join\n" + algebra_to_string(*node.left, indent + 1) +
                       algebra_to_string(*node.right, indent + 1);
            } else if constexpr (std::is_same_v<T, LeftJoinNode>) {
                std::string out = p + "LeftJoin";
                if (node.condition) out += " [" + expr_to_string(*node.condition) + "]";
                return out + "\n" + algebra_to_string(*node.left, indent + 1) +
                       algebra_to_string(*node.right, indent + 1);
            } else if constexpr (std::is_same_v<T, UnionNode>) {
                return p + "Union\n" + algebra_to_string(*node.left, indent + 1) +
                       algebra_to_string(*node.right, indent + 1);
            } else if constexpr (std::is_same_v<T, FilterNode>) {
                return p + "Filter " + expr_to_string(*node.condition) + "\n" +
                       algebra_to_string(*node.child, indent + 1);
            } else if constexpr (std::is_same_v<T, GraphNode>) {
                return p + "Graph " + node.graph.to_string() + "\n" +
                       algebra_to_string(*node.child, indent + 1);
            } else if constexpr (std::is_same_v<T, GroupNode>) {
                std::string out = p + "Group by [" + join_list(node.keys) + "]";
                for (const Aggregate& agg : node.aggregates) {
                    out += " " + std::string(aggregate_name(agg.func)) + "(";
                    if (agg.distinct) out += "DISTINCT ";
                    out += agg.star ? "*" : expr_to_string(*agg.argument);
                    out += ") AS ?" + agg.out_variable;
                }
                return out + "\n" + algebra_to_string(*node.child, indent + 1);
            } else if constexpr (std::is_same_v<T, ProjectNode>) {
                return p + "Project [" + join_list(node.variables) + "]\n" +
                       algebra_to_string(*node.child, indent + 1);
            } else if constexpr (std::is_same_v<T, DistinctNode>) {
                return p + "Distinct\n" + algebra_to_string(*node.child, indent + 1);
            } else if constexpr (std::is_same_v<T, OrderByNode>) {
                std::string out = p + "OrderBy [";
                for (std::size_t i = 0; i < node.keys.size(); ++i) {
                    if (i) out += ", ";
                    out += (node.keys[i].descending ? "DESC " : "ASC ") +
                           expr_to_string(*node.keys[i].expr);
                }
                return out + "]\n" + algebra_to_string(*node.child, indent + 1);
            } else {
                std::string out = p + "Slice offset=" + std::to_string(node.offset) +
                                  " limit=" +
                                  (node.limit < 0 ? std::string("none")
                                                  : std::to_string(node.limit));
                return out + "\n" + algebra_to_string(*node.child, indent + 1);
            }
        },
        algebra.node);
}

void collect_algebra_variables(const Algebra& algebra, std::vector<std::string>& out) {
    auto push = [&out](const std::string& name) {
        if (std::find(out.begin(), out.end(), name) == out.end()) out.push_back(name);
    };
    std::visit(
        [&](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, BgpNode>) {
                for (const TriplePattern& tp : node.patterns) {
                    for (const PatternTerm* t : {&tp.s, &tp.p, &tp.o}) {
                        if (t->is_variable) push(t->variable);
                    }
                }
            } else if constexpr (std::is_same_v<T, JoinNode> || std::is_same_v<T, UnionNode>) {
                collect_algebra_variables(*node.left, out);
                collect_algebra_variables(*node.right, out);
            } else if constexpr (std::is_same_v<T, LeftJoinNode>) {
                collect_algebra_variables(*node.left, out);
                collect_algebra_variables(*node.right, out);
                if (node.condition) collect_expr_variables(*node.condition, out);
            } else if constexpr (std::is_same_v<T, FilterNode>) {
                collect_algebra_variables(*node.child, out);
                collect_expr_variables(*node.condition, out);
            } else if constexpr (std::is_same_v<T, GraphNode>) {
                if (node.graph.is_variable) push(node.graph.variable);
                collect_algebra_variables(*node.child, out);
            } else if constexpr (std::is_same_v<T, GroupNode>) {
                collect_algebra_variables(*node.child, out);
                for (const Aggregate& agg : node.aggregates) push(agg.out_variable);
            } else if constexpr (std::is_same_v<T, OrderByNode>) {
                collect_algebra_variables(*node.child, out);
                for (const OrderKey& key : node.keys) collect_expr_variables(*key.expr, out);
            } else {
                collect_algebra_variables(*node.child, out);
            }
        },
        algebra.node);
}

}  // namespace trident
