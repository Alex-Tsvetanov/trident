// Evaluation of FILTER expressions and the comparison order used by ORDER BY.
#pragma once

#include <string>
#include <vector>

#include "trident/dictionary.hpp"
#include "trident/sparql_ast.hpp"
#include "trident/term.hpp"

namespace trident {

// A solution mapping. Variables are compiled to slots before execution starts,
// so a row is a flat vector and a variable lookup is an array index.
using Row = std::vector<TermId>;

// The result of evaluating an expression. SPARQL distinguishes an unbound
// variable from a type error, and a filter treats both as "not true", so the
// distinction has to survive evaluation.
struct Value {
    enum class Kind { Unbound, Error, Bound };
    Kind kind = Kind::Unbound;
    Term term;

    static Value unbound() { return Value{}; }
    static Value error() { return Value{Kind::Error, Term{}}; }
    static Value bound(Term t) { return Value{Kind::Bound, std::move(t)}; }
    static Value boolean(bool b) {
        return bound(Term::typed_literal(b ? "true" : "false", std::string(xsd::kBoolean)));
    }
    static Value number(double d);
    static Value integer(long long v) {
        return bound(Term::typed_literal(std::to_string(v), std::string(xsd::kInteger)));
    }

    bool is_bound() const { return kind == Kind::Bound; }
};

// True when the term is a literal with a numeric datatype, and writes its value.
bool numeric_value(const Term& term, double& out);
// True when the term is a literal that a string function may operate on.
bool string_value(const Term& term, std::string& out);

// The effective boolean value. Returns false for unbound, error and every term
// that has no boolean reading, which is what FILTER needs.
bool effective_boolean(const Value& value);

// SPARQL ordering: unbound first, then blank nodes, IRIs and literals, with
// numeric literals compared as numbers. Returns a negative, zero or positive
// value the way a three way comparison does.
int compare_terms(const Term& a, const Term& b);
int compare_values(const Value& a, const Value& b);

Value evaluate(const Expr& expr, const Row& row, const Dictionary& dictionary);

// Fills in the slot of every variable in the expression. Called once, at plan
// time, so that evaluation never has to look a variable up by name.
void resolve_expression_slots(Expr& expr, const std::vector<std::string>& variables);

}  // namespace trident
