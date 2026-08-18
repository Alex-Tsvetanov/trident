// The SPARQL algebra tree. The node types are gathered in one variant rather
// than in a hierarchy with virtual methods, because several different walks run
// over the tree (printing, planning, execution) and none of them is the tree's
// own business.
#pragma once

#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "trident/lexer.hpp"
#include "trident/term.hpp"

namespace trident {

// A position in a triple pattern: either a variable or a constant term.
struct PatternTerm {
    bool is_variable = false;
    std::string variable;  // without the leading question mark
    Term constant;

    static PatternTerm var(std::string name) {
        PatternTerm t;
        t.is_variable = true;
        t.variable = std::move(name);
        return t;
    }
    static PatternTerm value(Term term) {
        PatternTerm t;
        t.constant = std::move(term);
        return t;
    }
    std::string to_string() const {
        return is_variable ? "?" + variable : constant.to_display();
    }
};

struct TriplePattern {
    PatternTerm s, p, o;
    std::string to_string() const {
        return s.to_string() + " " + p.to_string() + " " + o.to_string();
    }
};

// --- expressions ---------------------------------------------------------

enum class ExprOp {
    Or, And,
    Equal, NotEqual, Less, Greater, LessEqual, GreaterEqual,
    Add, Subtract, Multiply, Divide,
    Not, Negate,
};

const char* expr_op_name(ExprOp op);

struct Expr;
using ExprPtr = std::unique_ptr<Expr>;

// The slot is filled in by the planner, which is the only place that knows the
// variable numbering. Evaluation then never looks a variable up by name.
struct VarExpr { std::string name; int slot = -1; };
struct ConstExpr { Term term; };
struct OpExpr { ExprOp op; ExprPtr lhs; ExprPtr rhs; };  // rhs is null for unary
struct CallExpr { std::string name; std::vector<ExprPtr> args; };  // name is upper case

struct Expr {
    std::variant<VarExpr, ConstExpr, OpExpr, CallExpr> node;
};

ExprPtr make_expr(std::variant<VarExpr, ConstExpr, OpExpr, CallExpr> node);
std::string expr_to_string(const Expr& expr);
// Every variable the expression mentions, in order of first appearance.
void collect_expr_variables(const Expr& expr, std::vector<std::string>& out);

// --- aggregates ----------------------------------------------------------

enum class AggregateFunc { Count, Sum, Min, Max, Avg, SampleFirst };

const char* aggregate_name(AggregateFunc func);

struct Aggregate {
    AggregateFunc func = AggregateFunc::Count;
    bool distinct = false;
    bool star = false;   // COUNT(*)
    ExprPtr argument;    // null when star
    std::string out_variable;
};

// --- algebra -------------------------------------------------------------

struct Algebra;
using AlgebraPtr = std::unique_ptr<Algebra>;

struct BgpNode { std::vector<TriplePattern> patterns; };
struct JoinNode { AlgebraPtr left, right; };
struct LeftJoinNode { AlgebraPtr left, right; ExprPtr condition; };
struct UnionNode { AlgebraPtr left, right; };
struct FilterNode { AlgebraPtr child; ExprPtr condition; };

struct GroupNode {
    AlgebraPtr child;
    std::vector<std::string> keys;
    std::vector<Aggregate> aggregates;
};

struct ProjectNode { AlgebraPtr child; std::vector<std::string> variables; };
struct DistinctNode { AlgebraPtr child; };

struct OrderKey { ExprPtr expr; bool descending = false; };
struct OrderByNode { AlgebraPtr child; std::vector<OrderKey> keys; };

struct SliceNode {
    AlgebraPtr child;
    long long offset = 0;
    long long limit = -1;  // negative means no limit
};

struct Algebra {
    std::variant<BgpNode, JoinNode, LeftJoinNode, UnionNode, FilterNode, GroupNode,
                 ProjectNode, DistinctNode, OrderByNode, SliceNode>
        node;
};

template <typename T>
AlgebraPtr make_algebra(T&& node) {
    auto out = std::make_unique<Algebra>();
    out->node = std::forward<T>(node);
    return out;
}

// Indented rendering of the tree, one node per line. This is what the demo
// prints under the heading "algebra".
std::string algebra_to_string(const Algebra& algebra, int indent = 0);

// Every variable the subtree can bind, in order of first appearance.
void collect_algebra_variables(const Algebra& algebra, std::vector<std::string>& out);

// --- the parsed query ----------------------------------------------------

struct Query {
    AlgebraPtr root;
    std::vector<std::string> projection;  // column order for output
    bool select_star = false;
    PrefixMap prefixes;
    std::string text;
};

}  // namespace trident
