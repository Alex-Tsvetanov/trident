#include "trident/sparql_parser.hpp"

#include <algorithm>
#include <cctype>
#include <string>

namespace trident {

namespace {

constexpr std::string_view kRdfType = "http://www.w3.org/1999/02/22-rdf-syntax-ns#type";

std::string upper(std::string_view text) {
    std::string out(text);
    for (char& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

// The built in functions the engine implements. A call to anything else is a
// syntax error, which is better than accepting it and returning nothing.
bool is_known_function(const std::string& name) {
    static const char* kKnown[] = {
        "BOUND", "STR", "LANG", "DATATYPE", "ISIRI", "ISURI", "ISLITERAL", "ISBLANK",
        "ISNUMERIC", "STRLEN", "UCASE", "LCASE", "CONTAINS", "STRSTARTS", "STRENDS",
        "SUBSTR", "REGEX", "ABS", "CEIL", "FLOOR", "ROUND", "IF", "COALESCE", "SAMETERM",
        "LANGMATCHES"};
    for (const char* known : kKnown) {
        if (name == known) return true;
    }
    return false;
}

class SparqlParser {
public:
    explicit SparqlParser(std::string_view text) : reader_(text), text_(text) {}

    Query run() {
        prologue();
        Query query;
        query.text = std::string(text_);

        expect_keyword("SELECT");
        bool distinct = false;
        if (peek_keyword("DISTINCT")) {
            take_keyword();
            distinct = true;
        } else if (peek_keyword("REDUCED")) {
            take_keyword();
            // REDUCED permits but does not require duplicate removal. Treating it
            // as a no operation is a conforming implementation.
        }
        select_clause(query);

        if (peek_keyword("WHERE")) take_keyword();
        AlgebraPtr body = group_graph_pattern();

        // GROUP BY, then the aggregates named in the select clause.
        std::vector<std::string> group_keys;
        if (peek_keyword("GROUP")) {
            take_keyword();
            expect_keyword("BY");
            do {
                reader_.skip_ws();
                if (in().peek() != '?' && in().peek() != '$') {
                    fail("GROUP BY takes variables in this implementation");
                }
                group_keys.push_back(read_variable());
                reader_.skip_ws();
            } while (in().peek() == '?' || in().peek() == '$');
        }
        if (!aggregates_.empty() || !group_keys.empty()) {
            GroupNode group;
            group.child = std::move(body);
            group.keys = group_keys;
            group.aggregates = std::move(aggregates_);
            body = make_algebra(std::move(group));
        }

        if (peek_keyword("HAVING")) fail("HAVING is outside the supported subset");

        // ORDER BY sits below the projection so that it can sort on variables the
        // projection drops, which is what the specification requires.
        std::vector<OrderKey> order_keys;
        if (peek_keyword("ORDER")) {
            take_keyword();
            expect_keyword("BY");
            order_keys = order_condition_list();
        }

        long long limit = -1, offset = 0;
        for (;;) {
            if (peek_keyword("LIMIT")) {
                take_keyword();
                limit = read_integer();
            } else if (peek_keyword("OFFSET")) {
                take_keyword();
                offset = read_integer();
            } else {
                break;
            }
        }

        reader_.skip_ws();
        if (!in().eof()) fail("unexpected text after the end of the query");

        if (query.select_star) {
            collect_algebra_variables(*body, query.projection);
        }

        if (!order_keys.empty()) {
            OrderByNode order;
            order.child = std::move(body);
            order.keys = std::move(order_keys);
            body = make_algebra(std::move(order));
        }
        body = make_algebra(ProjectNode{std::move(body), query.projection});
        if (distinct) body = make_algebra(DistinctNode{std::move(body)});
        if (limit >= 0 || offset > 0) {
            body = make_algebra(SliceNode{std::move(body), offset, limit});
        }
        query.root = std::move(body);
        query.prefixes = reader_.prefixes();
        return query;
    }

private:
    CharStream& in() { return reader_.stream(); }
    [[noreturn]] void fail(const std::string& message) { reader_.fail(message); }

    // --- keyword handling -------------------------------------------------
    // Peeks the next word without consuming it, so that a keyword test never
    // eats a variable or an IRI by mistake.
    std::string peek_word() {
        reader_.skip_ws();
        std::string word;
        std::size_t ahead = 0;
        while (std::isalpha(in().peek(ahead))) {
            word += static_cast<char>(in().peek(ahead));
            ++ahead;
        }
        // A word followed by a colon is a prefixed name, never a keyword.
        if (in().peek(ahead) == ':') return std::string();
        return upper(word);
    }

    bool peek_keyword(const char* keyword) { return peek_word() == keyword; }

    void take_keyword() {
        reader_.skip_ws();
        while (std::isalpha(in().peek())) in().get();
    }

    void expect_keyword(const char* keyword) {
        if (!peek_keyword(keyword)) fail(std::string("expected the keyword ") + keyword);
        take_keyword();
    }

    void expect_char(char c, const char* what) {
        reader_.skip_ws();
        if (in().peek() != c) fail(std::string("expected ") + what);
        in().get();
    }

    std::string read_variable() {
        reader_.skip_ws();
        if (in().peek() != '?' && in().peek() != '$') fail("expected a variable");
        in().get();
        std::string name;
        while (std::isalnum(in().peek()) || in().peek() == '_') {
            name += static_cast<char>(in().get());
        }
        if (name.empty()) fail("empty variable name");
        return name;
    }

    long long read_integer() {
        reader_.skip_ws();
        std::string digits;
        while (std::isdigit(in().peek())) digits += static_cast<char>(in().get());
        if (digits.empty()) fail("expected a non negative integer");
        return std::stoll(digits);
    }

    // --- prologue ---------------------------------------------------------
    void prologue() {
        for (;;) {
            if (peek_keyword("PREFIX")) {
                take_keyword();
                reader_.skip_ws();
                std::string prefix;
                while (reader_.is_pname_char(in().peek()) && in().peek() != ':') {
                    prefix += static_cast<char>(in().get());
                }
                expect_char(':', "a colon after the prefix name");
                reader_.skip_ws();
                reader_.prefixes().set(prefix, reader_.read_iriref());
            } else if (peek_keyword("BASE")) {
                take_keyword();
                reader_.skip_ws();
                reader_.prefixes().base = reader_.read_iriref();
            } else {
                return;
            }
        }
    }

    // --- select clause ----------------------------------------------------
    void select_clause(Query& query) {
        reader_.skip_ws();
        if (in().peek() == '*') {
            in().get();
            query.select_star = true;
            return;
        }
        bool any = false;
        for (;;) {
            reader_.skip_ws();
            if (in().peek() == '?' || in().peek() == '$') {
                query.projection.push_back(read_variable());
                any = true;
                continue;
            }
            if (in().peek() == '(') {
                in().get();
                Aggregate agg = aggregate_expression();
                query.projection.push_back(agg.out_variable);
                aggregates_.push_back(std::move(agg));
                expect_char(')', "a closing parenthesis after the projection expression");
                any = true;
                continue;
            }
            break;
        }
        if (!any) fail("the select clause is empty");
    }

    Aggregate aggregate_expression() {
        std::string name = peek_word();
        Aggregate agg;
        if (name == "COUNT") agg.func = AggregateFunc::Count;
        else if (name == "SUM") agg.func = AggregateFunc::Sum;
        else if (name == "MIN") agg.func = AggregateFunc::Min;
        else if (name == "MAX") agg.func = AggregateFunc::Max;
        else if (name == "AVG") agg.func = AggregateFunc::Avg;
        else if (name == "SAMPLE") agg.func = AggregateFunc::SampleFirst;
        else fail("only aggregate expressions are supported in the select clause");
        take_keyword();
        expect_char('(', "an opening parenthesis after the aggregate name");
        reader_.skip_ws();
        if (peek_keyword("DISTINCT")) {
            take_keyword();
            agg.distinct = true;
        }
        reader_.skip_ws();
        if (in().peek() == '*') {
            in().get();
            agg.star = true;
            if (agg.func != AggregateFunc::Count) fail("only COUNT accepts a star argument");
        } else {
            agg.argument = expression();
        }
        expect_char(')', "a closing parenthesis after the aggregate argument");
        expect_keyword("AS");
        agg.out_variable = read_variable();
        return agg;
    }

    std::vector<OrderKey> order_condition_list() {
        std::vector<OrderKey> keys;
        for (;;) {
            reader_.skip_ws();
            bool descending = false;
            if (peek_keyword("ASC") || peek_keyword("DESC")) {
                descending = peek_keyword("DESC");
                take_keyword();
                expect_char('(', "an opening parenthesis after ASC or DESC");
                keys.push_back(OrderKey{expression(), descending});
                expect_char(')', "a closing parenthesis after the sort expression");
                continue;
            }
            if (in().peek() == '(') {
                in().get();
                keys.push_back(OrderKey{expression(), false});
                expect_char(')', "a closing parenthesis after the sort expression");
                continue;
            }
            if (in().peek() == '?' || in().peek() == '$') {
                keys.push_back(OrderKey{make_expr(VarExpr{read_variable()}), false});
                continue;
            }
            break;
        }
        if (keys.empty()) fail("ORDER BY needs at least one sort condition");
        return keys;
    }

    // --- graph patterns ---------------------------------------------------
    AlgebraPtr group_graph_pattern() {
        expect_char('{', "an opening brace");
        AlgebraPtr accumulated;
        std::vector<ExprPtr> filters;
        std::vector<TriplePattern> pending;

        auto flush_pending = [&]() {
            if (pending.empty()) return;
            AlgebraPtr bgp = make_algebra(BgpNode{std::move(pending)});
            pending.clear();
            accumulated = accumulated ? make_algebra(JoinNode{std::move(accumulated),
                                                             std::move(bgp)})
                                      : std::move(bgp);
        };

        for (;;) {
            reader_.skip_ws();
            int c = in().peek();
            if (c < 0) fail("unterminated group pattern");
            if (c == '}') {
                in().get();
                break;
            }
            if (c == '.') {
                in().get();
                continue;
            }
            if (peek_keyword("FILTER")) {
                take_keyword();
                filters.push_back(constraint());
                continue;
            }
            if (peek_keyword("OPTIONAL")) {
                take_keyword();
                flush_pending();
                AlgebraPtr right = group_graph_pattern();
                ExprPtr condition;
                // A filter at the top of the optional group becomes the left join
                // condition, which is the translation the specification gives.
                if (auto* filter = std::get_if<FilterNode>(&right->node)) {
                    condition = std::move(filter->condition);
                    AlgebraPtr inner = std::move(filter->child);
                    right = std::move(inner);
                }
                if (!accumulated) accumulated = make_algebra(BgpNode{});
                accumulated = make_algebra(
                    LeftJoinNode{std::move(accumulated), std::move(right), std::move(condition)});
                continue;
            }
            if (peek_keyword("BIND") || peek_keyword("VALUES") || peek_keyword("SERVICE") ||
                peek_keyword("GRAPH") || peek_keyword("MINUS")) {
                fail("this construct is outside the supported subset");
            }
            if (c == '{') {
                flush_pending();
                AlgebraPtr group = group_or_union();
                accumulated = accumulated
                                  ? make_algebra(JoinNode{std::move(accumulated), std::move(group)})
                                  : std::move(group);
                continue;
            }
            triples_block(pending);
        }
        flush_pending();
        if (!accumulated) accumulated = make_algebra(BgpNode{});
        for (ExprPtr& filter : filters) {
            accumulated = make_algebra(FilterNode{std::move(accumulated), std::move(filter)});
        }
        return accumulated;
    }

    AlgebraPtr group_or_union() {
        AlgebraPtr left = group_graph_pattern();
        while (peek_keyword("UNION")) {
            take_keyword();
            AlgebraPtr right = group_graph_pattern();
            left = make_algebra(UnionNode{std::move(left), std::move(right)});
        }
        return left;
    }

    // --- triple patterns --------------------------------------------------
    PatternTerm subject_or_object(bool allow_literal) {
        reader_.skip_ws();
        int c = in().peek();
        if (c == '?' || c == '$') return PatternTerm::var(read_variable());
        if (c == '<') return PatternTerm::value(Term::iri(reader_.read_iriref()));
        if (c == '_') {
            // A blank node in a query pattern behaves as a variable that cannot
            // be named in the projection, so that is exactly how it is compiled.
            return PatternTerm::var(hidden_variable(reader_.read_blank_label()));
        }
        if (c == '[') {
            in().get();
            reader_.skip_ws();
            if (in().peek() != ']') fail("only the empty form [] is supported here");
            in().get();
            return PatternTerm::var(hidden_variable("anon" + std::to_string(next_hidden_++)));
        }
        if (allow_literal && (c == '"' || c == '\'')) {
            return PatternTerm::value(reader_.read_literal());
        }
        if (allow_literal && (is_numeric_start(c) || c == 't' || c == 'f')) {
            std::size_t ahead = 0;
            while (reader_.is_pname_char(in().peek(ahead))) ++ahead;
            if (in().peek(ahead) != ':') {
                return PatternTerm::value(reader_.read_numeric_or_boolean());
            }
        }
        return PatternTerm::value(Term::iri(reader_.read_prefixed_name()));
    }

    PatternTerm predicate_position() {
        reader_.skip_ws();
        int c = in().peek();
        if (c == '?' || c == '$') return PatternTerm::var(read_variable());
        if (c == '<') return PatternTerm::value(Term::iri(reader_.read_iriref()));
        if (c == 'a' && !reader_.is_pname_char(in().peek(1))) {
            in().get();
            return PatternTerm::value(Term::iri(std::string(kRdfType)));
        }
        if (c == '/' || c == '^' || c == '*' || c == '+') {
            fail("property paths are outside the supported subset");
        }
        return PatternTerm::value(Term::iri(reader_.read_prefixed_name()));
    }

    void triples_block(std::vector<TriplePattern>& out) {
        PatternTerm subject = subject_or_object(false);
        for (;;) {
            PatternTerm predicate = predicate_position();
            for (;;) {
                PatternTerm object = subject_or_object(true);
                out.push_back(TriplePattern{subject, predicate, object});
                reader_.skip_ws();
                if (in().peek() == ',') {
                    in().get();
                    continue;
                }
                break;
            }
            reader_.skip_ws();
            if (in().peek() == ';') {
                while (in().peek() == ';') {
                    in().get();
                    reader_.skip_ws();
                }
                if (in().peek() == '.' || in().peek() == '}') break;
                continue;
            }
            break;
        }
        reader_.skip_ws();
        if (in().peek() == '.') in().get();
    }

    std::string hidden_variable(const std::string& label) {
        // The space cannot occur in a variable the user writes, so a hidden
        // variable can never be shadowed by one.
        return " bnode_" + label;
    }

    // --- expressions ------------------------------------------------------
    ExprPtr constraint() {
        reader_.skip_ws();
        if (in().peek() == '(') {
            in().get();
            ExprPtr expr = expression();
            expect_char(')', "a closing parenthesis after the filter expression");
            return expr;
        }
        // FILTER regex(?x, "a") and the like, without the outer parentheses.
        return expression();
    }

    ExprPtr expression() { return conditional_or(); }

    ExprPtr conditional_or() {
        ExprPtr left = conditional_and();
        for (;;) {
            reader_.skip_ws();
            if (in().peek() == '|' && in().peek(1) == '|') {
                in().skip(2);
                left = make_expr(OpExpr{ExprOp::Or, std::move(left), conditional_and()});
                continue;
            }
            return left;
        }
    }

    ExprPtr conditional_and() {
        ExprPtr left = relational();
        for (;;) {
            reader_.skip_ws();
            if (in().peek() == '&' && in().peek(1) == '&') {
                in().skip(2);
                left = make_expr(OpExpr{ExprOp::And, std::move(left), relational()});
                continue;
            }
            return left;
        }
    }

    ExprPtr relational() {
        ExprPtr left = additive();
        reader_.skip_ws();
        int c = in().peek(), d = in().peek(1);
        ExprOp op;
        int width = 1;
        if (c == '=') op = ExprOp::Equal;
        else if (c == '!' && d == '=') { op = ExprOp::NotEqual; width = 2; }
        else if (c == '<' && d == '=') { op = ExprOp::LessEqual; width = 2; }
        else if (c == '>' && d == '=') { op = ExprOp::GreaterEqual; width = 2; }
        else if (c == '<') op = ExprOp::Less;
        else if (c == '>') op = ExprOp::Greater;
        else return left;
        in().skip(static_cast<std::size_t>(width));
        return make_expr(OpExpr{op, std::move(left), additive()});
    }

    ExprPtr additive() {
        ExprPtr left = multiplicative();
        for (;;) {
            reader_.skip_ws();
            int c = in().peek();
            if (c == '+' || c == '-') {
                // A sign glued to a digit belongs to the numeric literal, not to
                // the expression, but only when there is no space before it.
                in().get();
                ExprOp op = c == '+' ? ExprOp::Add : ExprOp::Subtract;
                left = make_expr(OpExpr{op, std::move(left), multiplicative()});
                continue;
            }
            return left;
        }
    }

    ExprPtr multiplicative() {
        ExprPtr left = unary();
        for (;;) {
            reader_.skip_ws();
            int c = in().peek();
            if (c == '*' || c == '/') {
                in().get();
                ExprOp op = c == '*' ? ExprOp::Multiply : ExprOp::Divide;
                left = make_expr(OpExpr{op, std::move(left), unary()});
                continue;
            }
            return left;
        }
    }

    ExprPtr unary() {
        reader_.skip_ws();
        int c = in().peek();
        if (c == '!') {
            in().get();
            return make_expr(OpExpr{ExprOp::Not, unary(), nullptr});
        }
        if (c == '-' && !std::isdigit(in().peek(1))) {
            in().get();
            return make_expr(OpExpr{ExprOp::Negate, unary(), nullptr});
        }
        if (c == '+') {
            in().get();
            return unary();
        }
        return primary();
    }

    ExprPtr primary() {
        reader_.skip_ws();
        int c = in().peek();
        if (c == '(') {
            in().get();
            ExprPtr inner = expression();
            expect_char(')', "a closing parenthesis in an expression");
            return inner;
        }
        if (c == '?' || c == '$') return make_expr(VarExpr{read_variable()});
        if (c == '"' || c == '\'') return make_expr(ConstExpr{reader_.read_literal()});
        if (c == '<') return make_expr(ConstExpr{Term::iri(reader_.read_iriref())});
        if (is_numeric_start(c)) {
            return make_expr(ConstExpr{reader_.read_numeric_or_boolean()});
        }
        std::string word = peek_word();
        if (!word.empty() && is_known_function(word)) {
            take_keyword();
            CallExpr call;
            call.name = word;
            expect_char('(', "an opening parenthesis after a function name");
            reader_.skip_ws();
            if (in().peek() == ')') {
                in().get();
                return make_expr(std::move(call));
            }
            for (;;) {
                call.args.push_back(expression());
                reader_.skip_ws();
                if (in().peek() == ',') {
                    in().get();
                    continue;
                }
                break;
            }
            expect_char(')', "a closing parenthesis after the function arguments");
            return make_expr(std::move(call));
        }
        if (word == "TRUE" || word == "FALSE") {
            take_keyword();
            return make_expr(ConstExpr{
                Term::typed_literal(word == "TRUE" ? "true" : "false", std::string(xsd::kBoolean))});
        }
        if (!word.empty()) fail("unknown function \"" + word + "\"");
        return make_expr(ConstExpr{Term::iri(reader_.read_prefixed_name())});
    }

    TermReader reader_;
    std::string_view text_;
    std::vector<Aggregate> aggregates_;
    int next_hidden_ = 0;
};

}  // namespace

Query parse_sparql(std::string_view text) {
    SparqlParser parser(text);
    return parser.run();
}

}  // namespace trident
