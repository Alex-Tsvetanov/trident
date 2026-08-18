// A locally written syntax corpus.
//
// The W3C publishes conformance suites for Turtle, N-Triples and SPARQL, but
// they are downloads, and this repository is meant to build and test with no
// network access at all. The cases below are therefore written here, following
// the shape of a conformance suite: every case is either positive, meaning the
// parser must accept it, or negative, meaning the parser must reject it with a
// ParseError that carries a position. Nothing is skipped silently: a case that
// the implementation cannot handle would fail here.
#include "harness.hpp"

#include <string>
#include <vector>

#include "trident/sparql_parser.hpp"
#include "trident/turtle.hpp"

using namespace trident;

namespace {

struct SyntaxCase {
    const char* name;
    const char* text;
    bool must_parse;
};

std::size_t parse_only(std::string_view text, bool ntriples_only = false) {
    TurtleOptions options;
    options.ntriples_only = ntriples_only;
    return parse_turtle(
        text, [](const Term&, const Term&, const Term&) {}, options);
}

// --- Turtle ------------------------------------------------------------

const SyntaxCase kTurtleCases[] = {
    {"turtle-positive-01-empty-document", "", true},
    {"turtle-positive-02-comment-only", "# just a comment\n", true},
    {"turtle-positive-03-iri-triple", "<http://a/s> <http://a/p> <http://a/o> .", true},
    {"turtle-positive-04-prefix", "@prefix p: <http://a/> .\np:s p:p p:o .", true},
    {"turtle-positive-05-empty-prefix", "@prefix : <http://a/> .\n:s :p :o .", true},
    {"turtle-positive-06-keyword-a", "@prefix p: <http://a/> .\np:s a p:C .", true},
    {"turtle-positive-07-predicate-list",
     "@prefix p: <http://a/> .\np:s p:p1 p:o1 ; p:p2 p:o2 .", true},
    {"turtle-positive-08-trailing-semicolon",
     "@prefix p: <http://a/> .\np:s p:p1 p:o1 ; .", true},
    {"turtle-positive-09-object-list",
     "@prefix p: <http://a/> .\np:s p:p p:o1 , p:o2 , p:o3 .", true},
    {"turtle-positive-10-langtag", "<http://a/s> <http://a/p> \"text\"@en-GB .", true},
    {"turtle-positive-11-datatype",
     "<http://a/s> <http://a/p> \"1\"^^<http://www.w3.org/2001/XMLSchema#integer> .", true},
    {"turtle-positive-12-numbers",
     "@prefix p: <http://a/> .\np:s p:p 1 , -2 , +3 , 4.5 , 6.7e8 , -9.0E-1 .", true},
    {"turtle-positive-13-booleans", "@prefix p: <http://a/> .\np:s p:p true , false .", true},
    {"turtle-positive-14-single-quotes", "@prefix p: <http://a/> .\np:s p:p 'text' .", true},
    {"turtle-positive-15-long-string",
     "@prefix p: <http://a/> .\np:s p:p \"\"\"a\nb\"\"\" .", true},
    {"turtle-positive-16-long-string-single",
     "@prefix p: <http://a/> .\np:s p:p '''a\nb''' .", true},
    {"turtle-positive-17-escapes",
     "<http://a/s> <http://a/p> \"tab\\there\\nnewline \\u00E9\" .", true},
    {"turtle-positive-18-blank-labels", "_:a <http://a/p> _:b .", true},
    {"turtle-positive-19-anon", "@prefix p: <http://a/> .\n[] p:p p:o .", true},
    {"turtle-positive-20-blank-property-list",
     "@prefix p: <http://a/> .\np:s p:p [ p:q p:r ] .", true},
    {"turtle-positive-21-nested-blank-property-list",
     "@prefix p: <http://a/> .\np:s p:p [ p:q [ p:r p:t ] ] .", true},
    {"turtle-positive-22-collection",
     "@prefix p: <http://a/> .\np:s p:p ( p:a p:b p:c ) .", true},
    {"turtle-positive-23-empty-collection", "@prefix p: <http://a/> .\np:s p:p ( ) .", true},
    {"turtle-positive-24-base", "@base <http://a/> .\n<s> <p> <o> .", true},
    {"turtle-positive-25-sparql-style-directives",
     "PREFIX p: <http://a/>\nBASE <http://a/>\np:s p:p <o> .", true},
    {"turtle-positive-26-local-name-with-dot",
     "@prefix p: <http://a/> .\np:s.1 p:p p:o.2 .", true},

    {"turtle-negative-01-missing-full-stop", "<http://a/s> <http://a/p> <http://a/o>", false},
    {"turtle-negative-02-undeclared-prefix", "p:s p:p p:o .", false},
    {"turtle-negative-03-unterminated-iri", "<http://a/s <http://a/p> <http://a/o> .", false},
    {"turtle-negative-04-unterminated-string",
     "<http://a/s> <http://a/p> \"open .", false},
    {"turtle-negative-05-newline-in-short-string",
     "<http://a/s> <http://a/p> \"a\nb\" .", false},
    {"turtle-negative-06-literal-as-subject", "\"s\" <http://a/p> <http://a/o> .", false},
    {"turtle-negative-07-literal-as-predicate",
     "<http://a/s> \"p\" <http://a/o> .", false},
    {"turtle-negative-08-empty-language-tag",
     "<http://a/s> <http://a/p> \"text\"@ .", false},
    {"turtle-negative-09-unknown-directive", "@nosuch <http://a/> .", false},
    {"turtle-negative-10-unclosed-collection",
     "@prefix p: <http://a/> .\np:s p:p ( p:a .", false},
    {"turtle-negative-11-unclosed-blank-property-list",
     "@prefix p: <http://a/> .\np:s p:p [ p:q p:r .", false},
    {"turtle-negative-12-space-in-iri",
     "<http://a/ s> <http://a/p> <http://a/o> .", false},
    {"turtle-negative-13-bad-exponent", "@prefix p: <http://a/> .\np:s p:p 1e .", false},
    {"turtle-negative-14-empty-blank-label", "_: <http://a/p> <http://a/o> .", false},
    {"turtle-negative-15-missing-object", "<http://a/s> <http://a/p> .", false},
};

// N-Triples is the subset of Turtle with no abbreviations. Every case that
// Turtle allows and N-Triples does not has to be rejected in strict mode.
const SyntaxCase kNTriplesCases[] = {
    {"ntriples-positive-01-iri-triple", "<http://a/s> <http://a/p> <http://a/o> .\n", true},
    {"ntriples-positive-02-literal", "<http://a/s> <http://a/p> \"text\" .\n", true},
    {"ntriples-positive-03-langtag", "<http://a/s> <http://a/p> \"text\"@en .\n", true},
    {"ntriples-positive-04-datatype",
     "<http://a/s> <http://a/p> \"1\"^^<http://www.w3.org/2001/XMLSchema#integer> .\n", true},
    {"ntriples-positive-05-blank-nodes", "_:a <http://a/p> _:b .\n", true},
    {"ntriples-positive-06-comment", "# comment\n<http://a/s> <http://a/p> <http://a/o> .\n",
     true},
    {"ntriples-positive-07-several-lines",
     "<http://a/s1> <http://a/p> <http://a/o> .\n<http://a/s2> <http://a/p> <http://a/o> .\n",
     true},

    {"ntriples-negative-01-prefix-directive", "@prefix p: <http://a/> .\n", false},
    {"ntriples-negative-02-keyword-a", "<http://a/s> a <http://a/o> .\n", false},
    {"ntriples-negative-03-predicate-list",
     "<http://a/s> <http://a/p> <http://a/o> ; <http://a/q> <http://a/r> .\n", false},
    {"ntriples-negative-04-object-list",
     "<http://a/s> <http://a/p> <http://a/o> , <http://a/r> .\n", false},
    {"ntriples-negative-05-bare-number", "<http://a/s> <http://a/p> 1 .\n", false},
    {"ntriples-negative-06-collection", "<http://a/s> <http://a/p> ( ) .\n", false},
    {"ntriples-negative-07-anon", "<http://a/s> <http://a/p> [ ] .\n", false},
    {"ntriples-negative-08-single-quotes", "<http://a/s> <http://a/p> 'text' .\n", false},
    {"ntriples-negative-09-long-string", "<http://a/s> <http://a/p> \"\"\"text\"\"\" .\n", false},
};

// --- SPARQL ------------------------------------------------------------

const SyntaxCase kSparqlCases[] = {
    {"sparql-positive-01-select-one-variable", "SELECT ?s WHERE { ?s ?p ?o }", true},
    {"sparql-positive-02-select-star", "SELECT * WHERE { ?s ?p ?o }", true},
    {"sparql-positive-03-distinct", "SELECT DISTINCT ?s WHERE { ?s ?p ?o }", true},
    {"sparql-positive-04-reduced", "SELECT REDUCED ?s WHERE { ?s ?p ?o }", true},
    {"sparql-positive-05-prefix",
     "PREFIX p: <http://a/> SELECT ?s WHERE { ?s p:q p:r }", true},
    {"sparql-positive-06-base", "BASE <http://a/> SELECT ?s WHERE { ?s <p> <o> }", true},
    {"sparql-positive-07-where-is-optional", "SELECT ?s { ?s ?p ?o }", true},
    {"sparql-positive-08-keyword-a", "SELECT ?s WHERE { ?s a <http://a/C> }", true},
    {"sparql-positive-09-predicate-object-list",
     "PREFIX p: <http://a/> SELECT ?s WHERE { ?s a p:C ; p:q ?o , ?o2 }", true},
    {"sparql-positive-10-filter-comparison",
     "SELECT ?s WHERE { ?s ?p ?o FILTER (?o > 3) }", true},
    {"sparql-positive-11-filter-logic",
     "SELECT ?s WHERE { ?s ?p ?o FILTER (?o > 3 && ?o < 9 || !BOUND(?o)) }", true},
    {"sparql-positive-12-filter-builtins",
     "SELECT ?s WHERE { ?s ?p ?o FILTER (STRLEN(STR(?o)) = 4) }", true},
    {"sparql-positive-13-filter-regex",
     "SELECT ?s WHERE { ?s ?p ?o FILTER REGEX(?o, \"^a\", \"i\") }", true},
    {"sparql-positive-14-optional",
     "SELECT * WHERE { ?s ?p ?o OPTIONAL { ?o ?q ?r } }", true},
    {"sparql-positive-15-union",
     "SELECT * WHERE { { ?s a <http://a/A> } UNION { ?s a <http://a/B> } }", true},
    {"sparql-positive-16-nested-group", "SELECT * WHERE { { ?s ?p ?o } }", true},
    {"sparql-positive-17-order-limit-offset",
     "SELECT ?s WHERE { ?s ?p ?o } ORDER BY ?o LIMIT 5 OFFSET 2", true},
    {"sparql-positive-18-order-desc",
     "SELECT ?s WHERE { ?s ?p ?o } ORDER BY DESC(?o) ASC(?s)", true},
    {"sparql-positive-19-group-by-count",
     "SELECT ?p (COUNT(?o) AS ?n) WHERE { ?s ?p ?o } GROUP BY ?p", true},
    {"sparql-positive-20-aggregates",
     "SELECT (SUM(?o) AS ?s1) (AVG(?o) AS ?s2) (MIN(?o) AS ?s3) (MAX(?o) AS ?s4) "
     "WHERE { ?x ?p ?o }", true},
    {"sparql-positive-21-count-star", "SELECT (COUNT(*) AS ?n) WHERE { ?s ?p ?o }", true},
    {"sparql-positive-22-count-distinct",
     "SELECT (COUNT(DISTINCT ?o) AS ?n) WHERE { ?s ?p ?o }", true},
    {"sparql-positive-23-blank-node-in-pattern",
     "SELECT ?o WHERE { [] <http://a/p> ?o }", true},
    {"sparql-positive-24-literals-in-pattern",
     "SELECT ?s WHERE { ?s <http://a/p> \"text\"@en . ?s <http://a/q> 42 }", true},
    {"sparql-positive-25-empty-pattern", "SELECT * WHERE { }", true},
    {"sparql-positive-26-dollar-variables", "SELECT $s WHERE { $s ?p ?o }", true},

    {"sparql-negative-01-no-select", "WHERE { ?s ?p ?o }", false},
    {"sparql-negative-02-empty-projection", "SELECT WHERE { ?s ?p ?o }", false},
    {"sparql-negative-03-unclosed-group", "SELECT * WHERE { ?s ?p ?o", false},
    {"sparql-negative-04-unclosed-filter",
     "SELECT * WHERE { ?s ?p ?o FILTER (?o > 3 }", false},
    {"sparql-negative-05-unknown-function",
     "SELECT * WHERE { ?s ?p ?o FILTER (NOSUCH(?o)) }", false},
    {"sparql-negative-06-undeclared-prefix", "SELECT * WHERE { ?s p:q ?o }", false},
    {"sparql-negative-07-ask-not-supported", "ASK { ?s ?p ?o }", false},
    {"sparql-negative-08-construct-not-supported",
     "CONSTRUCT { ?s ?p ?o } WHERE { ?s ?p ?o }", false},
    {"sparql-negative-09-describe-not-supported", "DESCRIBE <http://a/s>", false},
    {"sparql-negative-10-graph-not-supported",
     "SELECT * WHERE { GRAPH ?g { ?s ?p ?o } }", false},
    {"sparql-negative-11-bind-not-supported",
     "SELECT * WHERE { ?s ?p ?o BIND (1 AS ?x) }", false},
    {"sparql-negative-12-values-not-supported",
     "SELECT * WHERE { VALUES ?s { <http://a/x> } }", false},
    {"sparql-negative-13-having-not-supported",
     "SELECT ?p WHERE { ?s ?p ?o } GROUP BY ?p HAVING (COUNT(?o) > 1)", false},
    {"sparql-negative-14-property-path-not-supported",
     "SELECT * WHERE { ?s <http://a/p>/<http://a/q> ?o }", false},
    {"sparql-negative-15-trailing-text",
     "SELECT * WHERE { ?s ?p ?o } LIMIT 1 nonsense", false},
    {"sparql-negative-16-bad-limit", "SELECT * WHERE { ?s ?p ?o } LIMIT ?x", false},
    {"sparql-negative-17-aggregate-without-alias",
     "SELECT (COUNT(*)) WHERE { ?s ?p ?o }", false},
    {"sparql-negative-18-star-argument-to-sum",
     "SELECT (SUM(*) AS ?n) WHERE { ?s ?p ?o }", false},
};

struct Tally {
    int passed = 0, failed = 0;
    std::string failures;
};

Tally run_cases(const SyntaxCase* cases, std::size_t count, bool ntriples_only,
                bool sparql) {
    Tally tally;
    for (std::size_t i = 0; i < count; ++i) {
        const SyntaxCase& item = cases[i];
        bool parsed = true;
        std::string reason;
        try {
            if (sparql) {
                (void)parse_sparql(item.text);
            } else {
                (void)parse_only(item.text, ntriples_only);
            }
        } catch (const ParseError& error) {
            parsed = false;
            reason = error.what();
        }
        if (parsed == item.must_parse) {
            ++tally.passed;
        } else {
            ++tally.failed;
            tally.failures += std::string("\n        ") + item.name +
                              (item.must_parse ? " should have parsed: " + reason
                                               : " should have been rejected");
        }
    }
    return tally;
}

template <std::size_t N>
Tally run_cases(const SyntaxCase (&cases)[N], bool ntriples_only, bool sparql) {
    return run_cases(cases, N, ntriples_only, sparql);
}

}  // namespace

TEST(syntax_suite, turtle_corpus) {
    Tally tally = run_cases(kTurtleCases, false, false);
    if (tally.failed) ::testing::fail_at(__FILE__, __LINE__, tally.failures);
    CHECK_EQ(tally.passed, 41);
}

TEST(syntax_suite, ntriples_corpus) {
    Tally tally = run_cases(kNTriplesCases, true, false);
    if (tally.failed) ::testing::fail_at(__FILE__, __LINE__, tally.failures);
    CHECK_EQ(tally.passed, 16);
}

TEST(syntax_suite, sparql_corpus) {
    Tally tally = run_cases(kSparqlCases, false, true);
    if (tally.failed) ::testing::fail_at(__FILE__, __LINE__, tally.failures);
    CHECK_EQ(tally.passed, 44);
}

TEST(syntax_suite, every_rejection_carries_a_line_and_a_column) {
    // A parser that reports "syntax error" and nothing else is not much use, so
    // the position is part of the contract and is checked for every negative
    // case in the corpus.
    int checked = 0;
    auto examine = [&checked](const SyntaxCase& item, bool sparql, bool ntriples_only) {
        if (item.must_parse) return;
        try {
            if (sparql) {
                (void)parse_sparql(item.text);
            } else {
                (void)parse_only(item.text, ntriples_only);
            }
        } catch (const ParseError& error) {
            CHECK(error.line() >= 1);
            CHECK(error.column() >= 1);
            CHECK(!error.message().empty());
            ++checked;
        }
    };
    for (const SyntaxCase& item : kTurtleCases) examine(item, false, false);
    for (const SyntaxCase& item : kNTriplesCases) examine(item, false, true);
    for (const SyntaxCase& item : kSparqlCases) examine(item, true, false);
    CHECK_EQ(checked, 15 + 9 + 18);
}
