#include "harness.hpp"

#include <string>

#include "trident/sparql_parser.hpp"

using namespace trident;

namespace {

std::string algebra_of(const std::string& text) {
    Query query = parse_sparql(text);
    return algebra_to_string(*query.root);
}

bool has(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

const char* kPrefix = "PREFIX ex: <http://example.org/>\n";

}  // namespace

TEST(sparql_parser, projection_and_basic_graph_pattern) {
    Query query = parse_sparql(std::string(kPrefix) +
                               "SELECT ?s ?o WHERE { ?s ex:p ?o }");
    CHECK_EQ(query.projection.size(), std::size_t{2});
    CHECK_EQ(query.projection[0], std::string("s"));
    CHECK_EQ(query.projection[1], std::string("o"));
    std::string tree = algebra_to_string(*query.root);
    CHECK(has(tree, "Project [?s, ?o]"));
    CHECK(has(tree, "BGP"));
    CHECK(has(tree, "?s :p ?o"));
}

TEST(sparql_parser, select_star_projects_every_variable) {
    Query query = parse_sparql(std::string(kPrefix) +
                               "SELECT * WHERE { ?s ex:p ?o . ?o ex:q ?z }");
    CHECK(query.select_star);
    CHECK_EQ(query.projection.size(), std::size_t{3});
}

TEST(sparql_parser, distinct_wraps_the_projection) {
    std::string tree = algebra_of(std::string(kPrefix) + "SELECT DISTINCT ?s WHERE { ?s ex:p ?o }");
    CHECK(has(tree, "Distinct"));
    CHECK(tree.find("Distinct") < tree.find("Project"));
}

TEST(sparql_parser, predicate_object_lists_expand_to_separate_patterns) {
    std::string tree = algebra_of(std::string(kPrefix) +
                                  "SELECT ?s WHERE { ?s a ex:Thing ; ex:p ?o , ?o2 }");
    CHECK(has(tree, ":type :Thing"));
    CHECK(has(tree, "?s :p ?o\n"));
    CHECK(has(tree, "?s :p ?o2"));
}

TEST(sparql_parser, filter_becomes_a_filter_node_over_the_group) {
    std::string tree = algebra_of(std::string(kPrefix) +
                                  "SELECT ?s WHERE { ?s ex:age ?a . FILTER (?a > 30) }");
    CHECK(has(tree, "Filter (?a > "));
    CHECK(tree.find("Filter") < tree.find("BGP"));
}

TEST(sparql_parser, optional_becomes_a_left_join) {
    std::string tree = algebra_of(std::string(kPrefix) +
                                  "SELECT * WHERE { ?s ex:p ?o OPTIONAL { ?s ex:q ?z } }");
    CHECK(has(tree, "LeftJoin"));
}

TEST(sparql_parser, a_filter_inside_optional_becomes_the_join_condition) {
    // This is the translation the specification gives: the filter of an OPTIONAL
    // group is the condition of the left join, not a filter over its right side.
    std::string tree = algebra_of(std::string(kPrefix) +
                                  "SELECT * WHERE { ?s ex:p ?o OPTIONAL { ?s ex:q ?z "
                                  "FILTER (?z > 3) } }");
    CHECK(has(tree, "LeftJoin [(?z > "));
}

TEST(sparql_parser, union_of_two_groups) {
    std::string tree = algebra_of(std::string(kPrefix) +
                                  "SELECT * WHERE { { ?s a ex:A } UNION { ?s a ex:B } }");
    CHECK(has(tree, "Union"));
    CHECK(has(tree, ":type :A"));
    CHECK(has(tree, ":type :B"));
}

TEST(sparql_parser, order_by_limit_and_offset) {
    std::string tree = algebra_of(std::string(kPrefix) +
                                  "SELECT ?s WHERE { ?s ex:p ?o } ORDER BY DESC(?o) "
                                  "LIMIT 10 OFFSET 5");
    CHECK(has(tree, "Slice offset=5 limit=10"));
    CHECK(has(tree, "OrderBy [DESC ?o]"));
    // ORDER BY sits below the projection, so it can sort on a dropped variable.
    CHECK(tree.find("Project") < tree.find("OrderBy"));
}

TEST(sparql_parser, aggregates_and_group_by) {
    std::string tree = algebra_of(std::string(kPrefix) +
                                  "SELECT ?v (COUNT(?p) AS ?n) (AVG(?c) AS ?avg) WHERE "
                                  "{ ?p ex:venue ?v . ?p ex:count ?c } GROUP BY ?v");
    CHECK(has(tree, "Group by [?v]"));
    CHECK(has(tree, "COUNT(?p) AS ?n"));
    CHECK(has(tree, "AVG(?c) AS ?avg"));
}

TEST(sparql_parser, count_star_and_distinct_inside_an_aggregate) {
    std::string tree = algebra_of("SELECT (COUNT(*) AS ?n) (COUNT(DISTINCT ?o) AS ?d) "
                                  "WHERE { ?s ?p ?o }");
    CHECK(has(tree, "COUNT(*) AS ?n"));
    CHECK(has(tree, "COUNT(DISTINCT ?o) AS ?d"));
}

TEST(sparql_parser, operator_precedence_in_expressions) {
    std::string tree = algebra_of("SELECT ?s WHERE { ?s ?p ?o FILTER (?a + ?b * 2 = ?c || !?d) }");
    // Multiplication binds tighter than addition, comparison tighter than or.
    CHECK(has(tree, "(((?a + (?b * \"2\"^^xsd:integer)) = ?c) || !(?d))"));
}

TEST(sparql_parser, built_in_functions_are_parsed_as_calls) {
    std::string tree = algebra_of("SELECT ?s WHERE { ?s ?p ?o FILTER (BOUND(?o) && "
                                  "REGEX(STR(?o), \"^a\", \"i\")) }");
    CHECK(has(tree, "BOUND(?o)"));
    CHECK(has(tree, "REGEX(STR(?o), \"^a\", \"i\")"));
}

TEST(sparql_parser, blank_nodes_in_a_pattern_become_hidden_variables) {
    std::string tree = algebra_of(std::string(kPrefix) + "SELECT * WHERE { [] ex:p ?o }");
    // The hidden name contains a space, which no written variable can, and it is
    // not projected by SELECT *.
    Query query = parse_sparql(std::string(kPrefix) + "SELECT * WHERE { _:b ex:p ?o }");
    CHECK(has(tree, "? bnode_anon0"));
    CHECK_EQ(query.projection.size(), std::size_t{2});
}

TEST(sparql_parser, base_and_prefix_declarations_are_applied) {
    std::string tree = algebra_of("BASE <http://example.org/dir/>\n"
                                  "PREFIX p: <http://other.example/>\n"
                                  "SELECT * WHERE { <a> p:b ?o }");
    CHECK(has(tree, ":a :b ?o"));
    Query query = parse_sparql("BASE <http://example.org/dir/>\nSELECT * WHERE { <a> <b> ?o }");
    CHECK_EQ(query.prefixes.base, std::string("http://example.org/dir/"));
}

TEST(sparql_parser, unknown_function_is_rejected) {
    CHECK_THROWS(parse_sparql("SELECT ?s WHERE { ?s ?p ?o FILTER (NOSUCHFN(?o)) }"), ParseError);
}

TEST(sparql_parser, unsupported_constructs_are_rejected_explicitly) {
    CHECK_THROWS(parse_sparql("SELECT ?s WHERE { ?s ?p ?o } HAVING (COUNT(?o) > 1)"), ParseError);
    CHECK_THROWS(parse_sparql("SELECT ?s WHERE { ?s ?p ?o BIND (1 AS ?x) }"), ParseError);
}

TEST(sparql_parser, graph_clause_is_accepted) {
    Query query = parse_sparql("SELECT ?s WHERE { GRAPH ?g { ?s ?p ?o } }");
    CHECK(query.root != nullptr);
}

TEST(sparql_parser, syntax_errors_carry_a_position) {
    bool caught = false;
    try {
        parse_sparql("SELECT ?s\nWHERE { ?s ex:p ?o }");
    } catch (const ParseError& error) {
        caught = true;
        CHECK_EQ(error.line(), 2);
    }
    CHECK(caught);
    CHECK_THROWS(parse_sparql("ASK { ?s ?p ?o }"), ParseError);
    CHECK_THROWS(parse_sparql("SELECT ?s WHERE { ?s ?p ?o "), ParseError);
    CHECK_THROWS(parse_sparql("SELECT WHERE { ?s ?p ?o }"), ParseError);
}

TEST(sparql_parser, trailing_text_after_the_query_is_rejected) {
    CHECK_THROWS(parse_sparql("SELECT ?s WHERE { ?s ?p ?o } LIMIT 1 nonsense"), ParseError);
}
