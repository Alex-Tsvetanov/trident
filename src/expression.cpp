#include "trident/expression.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <regex>
#include <sstream>
#include <stdexcept>

namespace trident {

namespace {

bool is_numeric_datatype(const std::string& datatype) {
    return datatype == xsd::kInteger || datatype == xsd::kDecimal || datatype == xsd::kDouble ||
           datatype == "http://www.w3.org/2001/XMLSchema#float" ||
           datatype == "http://www.w3.org/2001/XMLSchema#long" ||
           datatype == "http://www.w3.org/2001/XMLSchema#int" ||
           datatype == "http://www.w3.org/2001/XMLSchema#short" ||
           datatype == "http://www.w3.org/2001/XMLSchema#byte" ||
           datatype == "http://www.w3.org/2001/XMLSchema#nonNegativeInteger" ||
           datatype == "http://www.w3.org/2001/XMLSchema#positiveInteger";
}

bool is_integer_datatype(const std::string& datatype) {
    return datatype == xsd::kInteger ||
           datatype == "http://www.w3.org/2001/XMLSchema#long" ||
           datatype == "http://www.w3.org/2001/XMLSchema#int" ||
           datatype == "http://www.w3.org/2001/XMLSchema#short" ||
           datatype == "http://www.w3.org/2001/XMLSchema#byte" ||
           datatype == "http://www.w3.org/2001/XMLSchema#nonNegativeInteger" ||
           datatype == "http://www.w3.org/2001/XMLSchema#positiveInteger";
}

int kind_rank(const Term& term) {
    switch (term.kind) {
        case TermKind::Blank: return 1;
        case TermKind::Iri: return 2;
        case TermKind::Literal: return 3;
        default: return 0;
    }
}

Value number_result(double value, bool integral) {
    if (integral && std::abs(value) < 9.0e15 && value == std::floor(value)) {
        return Value::integer(static_cast<long long>(value));
    }
    std::ostringstream out;
    out.precision(15);
    out << value;
    return Value::bound(Term::typed_literal(out.str(), std::string(xsd::kDouble)));
}

}  // namespace

Value Value::number(double d) { return number_result(d, false); }

bool numeric_value(const Term& term, double& out) {
    if (term.kind != TermKind::Literal) return false;
    if (!is_numeric_datatype(term.datatype)) return false;
    // Integers are the overwhelming majority in real data, and they are parsed
    // here without strtod. That matters: ORDER BY and FILTER call this once per
    // comparison, and strtod is locale aware and an order of magnitude slower.
    if (is_integer_datatype(term.datatype)) {
        const std::string& text = term.value;
        std::size_t i = 0;
        bool negative = false;
        if (i < text.size() && (text[i] == '+' || text[i] == '-')) {
            negative = text[i] == '-';
            ++i;
        }
        if (i < text.size() && text.size() - i <= 18) {
            long long value = 0;
            for (; i < text.size(); ++i) {
                if (text[i] < '0' || text[i] > '9') break;
                value = value * 10 + (text[i] - '0');
            }
            if (i == text.size()) {
                out = static_cast<double>(negative ? -value : value);
                return true;
            }
        }
    }
    try {
        std::size_t consumed = 0;
        out = std::stod(term.value, &consumed);
        return consumed == term.value.size();
    } catch (const std::exception&) {
        return false;
    }
}

bool string_value(const Term& term, std::string& out) {
    if (term.kind != TermKind::Literal) return false;
    if (!term.language.empty() || term.datatype == xsd::kString || term.datatype.empty()) {
        out = term.value;
        return true;
    }
    return false;
}

bool effective_boolean(const Value& value) {
    if (!value.is_bound()) return false;
    const Term& term = value.term;
    if (term.kind != TermKind::Literal) return false;
    if (term.datatype == xsd::kBoolean) return term.value == "true" || term.value == "1";
    double number = 0;
    if (numeric_value(term, number)) return number != 0 && !std::isnan(number);
    std::string text;
    if (string_value(term, text)) return !text.empty();
    return false;
}

int compare_terms(const Term& a, const Term& b) {
    int ra = kind_rank(a), rb = kind_rank(b);
    if (ra != rb) return ra < rb ? -1 : 1;
    if (a.kind == TermKind::Literal) {
        double na = 0, nb = 0;
        if (numeric_value(a, na) && numeric_value(b, nb)) {
            if (na < nb) return -1;
            if (na > nb) return 1;
            return 0;
        }
    }
    if (a.value != b.value) return a.value < b.value ? -1 : 1;
    if (a.language != b.language) return a.language < b.language ? -1 : 1;
    if (a.datatype != b.datatype) return a.datatype < b.datatype ? -1 : 1;
    return 0;
}

int compare_values(const Value& a, const Value& b) {
    if (!a.is_bound() && !b.is_bound()) return 0;
    if (!a.is_bound()) return -1;
    if (!b.is_bound()) return 1;
    return compare_terms(a.term, b.term);
}

namespace {

// Equality follows the specification closely enough for the supported subset:
// numbers compare as numbers, strings as strings, everything else by term.
Value equality(const Value& a, const Value& b, bool negate) {
    if (!a.is_bound() || !b.is_bound()) return Value::error();
    double na = 0, nb = 0;
    bool result;
    if (numeric_value(a.term, na) && numeric_value(b.term, nb)) {
        result = na == nb;
    } else {
        result = a.term == b.term;
    }
    return Value::boolean(negate ? !result : result);
}

Value ordering(const Value& a, const Value& b, ExprOp op) {
    if (!a.is_bound() || !b.is_bound()) return Value::error();
    double na = 0, nb = 0;
    int cmp;
    if (numeric_value(a.term, na) && numeric_value(b.term, nb)) {
        cmp = na < nb ? -1 : (na > nb ? 1 : 0);
    } else {
        std::string sa, sb;
        bool a_str = string_value(a.term, sa), b_str = string_value(b.term, sb);
        if (a_str && b_str) {
            cmp = sa < sb ? -1 : (sa > sb ? 1 : 0);
        } else if (a.term.kind == TermKind::Iri && b.term.kind == TermKind::Iri) {
            cmp = a.term.value < b.term.value ? -1 : (a.term.value > b.term.value ? 1 : 0);
        } else {
            return Value::error();
        }
    }
    switch (op) {
        case ExprOp::Less: return Value::boolean(cmp < 0);
        case ExprOp::Greater: return Value::boolean(cmp > 0);
        case ExprOp::LessEqual: return Value::boolean(cmp <= 0);
        case ExprOp::GreaterEqual: return Value::boolean(cmp >= 0);
        default: return Value::error();
    }
}

Value arithmetic(const Value& a, const Value& b, ExprOp op) {
    double na = 0, nb = 0;
    if (!a.is_bound() || !b.is_bound()) return Value::error();
    if (!numeric_value(a.term, na) || !numeric_value(b.term, nb)) return Value::error();
    bool integral = is_integer_datatype(a.term.datatype) && is_integer_datatype(b.term.datatype);
    switch (op) {
        case ExprOp::Add: return number_result(na + nb, integral);
        case ExprOp::Subtract: return number_result(na - nb, integral);
        case ExprOp::Multiply: return number_result(na * nb, integral);
        case ExprOp::Divide:
            if (nb == 0) return Value::error();
            return number_result(na / nb, false);
        default: return Value::error();
    }
}

Value call_function(const CallExpr& call, const Row& row, const Dictionary& dictionary);

Value eval(const Expr& expr, const Row& row, const Dictionary& dictionary) {
    return std::visit(
        [&](const auto& node) -> Value {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, VarExpr>) {
                if (node.slot < 0 || static_cast<std::size_t>(node.slot) >= row.size()) {
                    return Value::unbound();
                }
                TermId id = row[static_cast<std::size_t>(node.slot)];
                if (!id.valid()) return Value::unbound();
                return Value::bound(dictionary.decode(id));
            } else if constexpr (std::is_same_v<T, ConstExpr>) {
                return Value::bound(node.term);
            } else if constexpr (std::is_same_v<T, OpExpr>) {
                switch (node.op) {
                    case ExprOp::Or: {
                        // Three valued: an error on one side is absorbed when the
                        // other side is true.
                        Value left = eval(*node.lhs, row, dictionary);
                        if (effective_boolean(left)) return Value::boolean(true);
                        Value right = eval(*node.rhs, row, dictionary);
                        if (effective_boolean(right)) return Value::boolean(true);
                        if (left.kind == Value::Kind::Error || right.kind == Value::Kind::Error) {
                            return Value::error();
                        }
                        return Value::boolean(false);
                    }
                    case ExprOp::And: {
                        Value left = eval(*node.lhs, row, dictionary);
                        if (left.is_bound() && !effective_boolean(left)) {
                            return Value::boolean(false);
                        }
                        Value right = eval(*node.rhs, row, dictionary);
                        if (right.is_bound() && !effective_boolean(right)) {
                            return Value::boolean(false);
                        }
                        if (left.kind == Value::Kind::Error || right.kind == Value::Kind::Error ||
                            !left.is_bound() || !right.is_bound()) {
                            return Value::error();
                        }
                        return Value::boolean(true);
                    }
                    case ExprOp::Not: {
                        Value inner = eval(*node.lhs, row, dictionary);
                        if (inner.kind == Value::Kind::Error) return Value::error();
                        return Value::boolean(!effective_boolean(inner));
                    }
                    case ExprOp::Negate: {
                        Value inner = eval(*node.lhs, row, dictionary);
                        double n = 0;
                        if (!inner.is_bound() || !numeric_value(inner.term, n)) {
                            return Value::error();
                        }
                        return number_result(-n, is_integer_datatype(inner.term.datatype));
                    }
                    case ExprOp::Equal:
                    case ExprOp::NotEqual:
                        return equality(eval(*node.lhs, row, dictionary),
                                        eval(*node.rhs, row, dictionary),
                                        node.op == ExprOp::NotEqual);
                    case ExprOp::Less:
                    case ExprOp::Greater:
                    case ExprOp::LessEqual:
                    case ExprOp::GreaterEqual:
                        return ordering(eval(*node.lhs, row, dictionary),
                                        eval(*node.rhs, row, dictionary), node.op);
                    default:
                        return arithmetic(eval(*node.lhs, row, dictionary),
                                          eval(*node.rhs, row, dictionary), node.op);
                }
            } else {
                return call_function(node, row, dictionary);
            }
        },
        expr.node);
}

Value call_function(const CallExpr& call, const Row& row, const Dictionary& dictionary) {
    const std::string& name = call.name;
    auto arg = [&](std::size_t i) { return eval(*call.args[i], row, dictionary); };
    auto arity = [&](std::size_t n) { return call.args.size() == n; };

    if (name == "BOUND") {
        if (!arity(1)) return Value::error();
        // BOUND inspects the binding itself, so it must not go through eval,
        // which would collapse unbound and error into the same answer.
        if (const auto* var = std::get_if<VarExpr>(&call.args[0]->node)) {
            if (var->slot < 0 || static_cast<std::size_t>(var->slot) >= row.size()) {
                return Value::boolean(false);
            }
            return Value::boolean(row[static_cast<std::size_t>(var->slot)].valid());
        }
        return Value::boolean(arg(0).is_bound());
    }
    if (name == "COALESCE") {
        for (std::size_t i = 0; i < call.args.size(); ++i) {
            Value v = arg(i);
            if (v.is_bound()) return v;
        }
        return Value::error();
    }
    if (name == "IF") {
        if (!arity(3)) return Value::error();
        return effective_boolean(arg(0)) ? arg(1) : arg(2);
    }

    if (call.args.empty()) return Value::error();
    Value first = arg(0);

    if (name == "ISIRI" || name == "ISURI") {
        return first.is_bound() ? Value::boolean(first.term.kind == TermKind::Iri)
                                : Value::error();
    }
    if (name == "ISLITERAL") {
        return first.is_bound() ? Value::boolean(first.term.kind == TermKind::Literal)
                                : Value::error();
    }
    if (name == "ISBLANK") {
        return first.is_bound() ? Value::boolean(first.term.kind == TermKind::Blank)
                                : Value::error();
    }
    if (name == "ISNUMERIC") {
        double n = 0;
        return first.is_bound() ? Value::boolean(numeric_value(first.term, n)) : Value::error();
    }
    if (name == "STR") {
        if (!first.is_bound()) return Value::error();
        return Value::bound(Term::literal(first.term.value));
    }
    if (name == "LANG") {
        if (!first.is_bound() || first.term.kind != TermKind::Literal) return Value::error();
        return Value::bound(Term::literal(first.term.language));
    }
    if (name == "DATATYPE") {
        if (!first.is_bound() || first.term.kind != TermKind::Literal) return Value::error();
        return Value::bound(Term::iri(first.term.datatype.empty() ? std::string(xsd::kString)
                                                                  : first.term.datatype));
    }
    if (name == "SAMETERM") {
        Value second = arg(1);
        if (!first.is_bound() || !second.is_bound()) return Value::error();
        return Value::boolean(first.term == second.term);
    }
    if (name == "LANGMATCHES") {
        Value second = arg(1);
        std::string tag, range;
        if (!first.is_bound() || !second.is_bound()) return Value::error();
        if (!string_value(first.term, tag) || !string_value(second.term, range)) {
            return Value::error();
        }
        if (range == "*") return Value::boolean(!tag.empty());
        auto lower = [](std::string s) {
            for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        };
        tag = lower(tag);
        range = lower(range);
        return Value::boolean(tag == range ||
                              (tag.size() > range.size() && tag.compare(0, range.size(), range) == 0 &&
                               tag[range.size()] == '-'));
    }

    // String functions.
    std::string text;
    if (name == "STRLEN" || name == "UCASE" || name == "LCASE" || name == "CONTAINS" ||
        name == "STRSTARTS" || name == "STRENDS" || name == "SUBSTR" || name == "REGEX") {
        if (!first.is_bound() || !string_value(first.term, text)) return Value::error();
    }
    if (name == "STRLEN") return Value::integer(static_cast<long long>(text.size()));
    if (name == "UCASE" || name == "LCASE") {
        for (char& c : text) {
            c = static_cast<char>(name == "UCASE"
                                      ? std::toupper(static_cast<unsigned char>(c))
                                      : std::tolower(static_cast<unsigned char>(c)));
        }
        Term out = first.term;
        out.value = text;
        return Value::bound(out);
    }
    if (name == "CONTAINS" || name == "STRSTARTS" || name == "STRENDS") {
        Value second = arg(1);
        std::string needle;
        if (!second.is_bound() || !string_value(second.term, needle)) return Value::error();
        if (name == "CONTAINS") return Value::boolean(text.find(needle) != std::string::npos);
        if (name == "STRSTARTS") {
            return Value::boolean(text.size() >= needle.size() &&
                                  text.compare(0, needle.size(), needle) == 0);
        }
        return Value::boolean(text.size() >= needle.size() &&
                              text.compare(text.size() - needle.size(), needle.size(), needle) == 0);
    }
    if (name == "SUBSTR") {
        Value second = arg(1);
        double start = 0;
        if (!second.is_bound() || !numeric_value(second.term, start)) return Value::error();
        std::size_t from = start <= 1 ? 0 : static_cast<std::size_t>(start) - 1;
        if (from >= text.size()) return Value::bound(Term::literal(""));
        std::size_t length = text.size() - from;
        if (call.args.size() >= 3) {
            Value third = arg(2);
            double count = 0;
            if (!third.is_bound() || !numeric_value(third.term, count)) return Value::error();
            length = std::min(length, static_cast<std::size_t>(std::max(0.0, count)));
        }
        Term out = first.term;
        out.value = text.substr(from, length);
        return Value::bound(out);
    }
    if (name == "REGEX") {
        Value second = arg(1);
        std::string pattern;
        if (!second.is_bound() || !string_value(second.term, pattern)) return Value::error();
        auto flags = std::regex::ECMAScript;
        if (call.args.size() >= 3) {
            Value third = arg(2);
            std::string modifiers;
            if (third.is_bound() && string_value(third.term, modifiers) &&
                modifiers.find('i') != std::string::npos) {
                flags |= std::regex::icase;
            }
        }
        try {
            std::regex re(pattern, flags);
            return Value::boolean(std::regex_search(text, re));
        } catch (const std::regex_error&) {
            return Value::error();
        }
    }

    // Numeric functions.
    double number = 0;
    if (!first.is_bound() || !numeric_value(first.term, number)) return Value::error();
    bool integral = is_integer_datatype(first.term.datatype);
    if (name == "ABS") return number_result(std::abs(number), integral);
    if (name == "CEIL") return number_result(std::ceil(number), true);
    if (name == "FLOOR") return number_result(std::floor(number), true);
    if (name == "ROUND") return number_result(std::floor(number + 0.5), true);
    return Value::error();
}

}  // namespace

Value evaluate(const Expr& expr, const Row& row, const Dictionary& dictionary) {
    return eval(expr, row, dictionary);
}

void resolve_expression_slots(Expr& expr, const std::vector<std::string>& variables) {
    std::visit(
        [&](auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, VarExpr>) {
                auto it = std::find(variables.begin(), variables.end(), node.name);
                node.slot = it == variables.end()
                                ? -1
                                : static_cast<int>(it - variables.begin());
            } else if constexpr (std::is_same_v<T, OpExpr>) {
                if (node.lhs) resolve_expression_slots(*node.lhs, variables);
                if (node.rhs) resolve_expression_slots(*node.rhs, variables);
            } else if constexpr (std::is_same_v<T, CallExpr>) {
                for (ExprPtr& arg : node.args) resolve_expression_slots(*arg, variables);
            }
        },
        expr.node);
}

}  // namespace trident
