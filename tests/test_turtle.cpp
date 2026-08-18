#include "harness.hpp"

#include <algorithm>
#include <string>
#include <vector>

#include "trident/turtle.hpp"

using namespace trident;

namespace {

std::vector<std::string> parse_to_lines(std::string_view text,
                                        const TurtleOptions& options = {}) {
    std::vector<std::string> out;
    parse_turtle(
        text,
        [&](const Term& s, const Term& p, const Term& o) {
            out.push_back(s.to_ntriples() + " " + p.to_ntriples() + " " + o.to_ntriples());
        },
        options);
    return out;
}

bool contains(const std::vector<std::string>& lines, const std::string& what) {
    return std::find(lines.begin(), lines.end(), what) != lines.end();
}

}  // namespace

TEST(turtle, ntriples_basic_triple) {
    auto lines = parse_to_lines("<http://a/s> <http://a/p> <http://a/o> .\n");
    CHECK_EQ(lines.size(), std::size_t{1});
    CHECK_EQ(lines[0], std::string("<http://a/s> <http://a/p> <http://a/o>"));
}

TEST(turtle, ntriples_literal_forms) {
    auto lines = parse_to_lines(
        "<http://a/s> <http://a/p> \"plain\" .\n"
        "<http://a/s> <http://a/p> \"tagged\"@BG .\n"
        "<http://a/s> <http://a/p> \"7\"^^<http://www.w3.org/2001/XMLSchema#integer> .\n");
    CHECK_EQ(lines.size(), std::size_t{3});
    CHECK(contains(lines, "<http://a/s> <http://a/p> \"plain\""));
    // The language tag is normalised to lower case, as RDF 1.1 requires.
    CHECK(contains(lines, "<http://a/s> <http://a/p> \"tagged\"@bg"));
    CHECK(contains(lines,
                   "<http://a/s> <http://a/p> "
                   "\"7\"^^<http://www.w3.org/2001/XMLSchema#integer>"));
}

TEST(turtle, prefixes_and_a_keyword) {
    auto lines = parse_to_lines(
        "@prefix ex: <http://example.org/> .\n"
        "ex:alice a ex:Person .\n");
    CHECK_EQ(lines.size(), std::size_t{1});
    CHECK_EQ(lines[0],
             std::string("<http://example.org/alice> "
                         "<http://www.w3.org/1999/02/22-rdf-syntax-ns#type> "
                         "<http://example.org/Person>"));
}

TEST(turtle, sparql_style_prefix_keyword) {
    auto lines = parse_to_lines(
        "PREFIX ex: <http://example.org/>\n"
        "ex:a ex:b ex:c .\n");
    CHECK_EQ(lines.size(), std::size_t{1});
    CHECK_EQ(lines[0], std::string("<http://example.org/a> <http://example.org/b> "
                                   "<http://example.org/c>"));
}

TEST(turtle, predicate_and_object_lists) {
    auto lines = parse_to_lines(
        "@prefix ex: <http://example.org/> .\n"
        "ex:alice a ex:Person ;\n"
        "         ex:knows ex:bob, ex:carol ;\n"
        "         ex:age 33 .\n");
    CHECK_EQ(lines.size(), std::size_t{4});
    CHECK(contains(lines, "<http://example.org/alice> <http://example.org/knows> "
                          "<http://example.org/bob>"));
    CHECK(contains(lines, "<http://example.org/alice> <http://example.org/knows> "
                          "<http://example.org/carol>"));
    CHECK(contains(lines, "<http://example.org/alice> <http://example.org/age> "
                          "\"33\"^^<http://www.w3.org/2001/XMLSchema#integer>"));
}

TEST(turtle, numeric_and_boolean_literals) {
    auto lines = parse_to_lines(
        "@prefix ex: <http://example.org/> .\n"
        "ex:s ex:int 42 ; ex:dec -1.5 ; ex:dbl 6.02e23 ; ex:flag true .\n");
    CHECK_EQ(lines.size(), std::size_t{4});
    CHECK(contains(lines, "<http://example.org/s> <http://example.org/int> "
                          "\"42\"^^<http://www.w3.org/2001/XMLSchema#integer>"));
    CHECK(contains(lines, "<http://example.org/s> <http://example.org/dec> "
                          "\"-1.5\"^^<http://www.w3.org/2001/XMLSchema#decimal>"));
    CHECK(contains(lines, "<http://example.org/s> <http://example.org/dbl> "
                          "\"6.02e23\"^^<http://www.w3.org/2001/XMLSchema#double>"));
    CHECK(contains(lines, "<http://example.org/s> <http://example.org/flag> "
                          "\"true\"^^<http://www.w3.org/2001/XMLSchema#boolean>"));
}

TEST(turtle, blank_node_property_list) {
    auto lines = parse_to_lines(
        "@prefix ex: <http://example.org/> .\n"
        "ex:alice ex:address [ ex:city \"Sofia\" ; ex:zip \"1000\" ] .\n");
    CHECK_EQ(lines.size(), std::size_t{3});
    // The anonymous node is both the object of ex:address and the subject of the
    // two nested statements.
    CHECK(contains(lines, "<http://example.org/alice> <http://example.org/address> _:genid0"));
    CHECK(contains(lines, "_:genid0 <http://example.org/city> \"Sofia\""));
    CHECK(contains(lines, "_:genid0 <http://example.org/zip> \"1000\""));
}

TEST(turtle, collection_expands_to_first_rest_chain) {
    auto lines = parse_to_lines(
        "@prefix ex: <http://example.org/> .\n"
        "ex:s ex:list ( ex:a ex:b ) .\n");
    CHECK_EQ(lines.size(), std::size_t{5});
    CHECK(contains(lines, "_:genid0 <http://www.w3.org/1999/02/22-rdf-syntax-ns#first> "
                          "<http://example.org/a>"));
    CHECK(contains(lines, "_:genid0 <http://www.w3.org/1999/02/22-rdf-syntax-ns#rest> _:genid1"));
    CHECK(contains(lines, "_:genid1 <http://www.w3.org/1999/02/22-rdf-syntax-ns#rest> "
                          "<http://www.w3.org/1999/02/22-rdf-syntax-ns#nil>"));
}

TEST(turtle, empty_collection_is_nil) {
    auto lines = parse_to_lines(
        "@prefix ex: <http://example.org/> .\n"
        "ex:s ex:list ( ) .\n");
    CHECK_EQ(lines.size(), std::size_t{1});
    CHECK_EQ(lines[0], std::string("<http://example.org/s> <http://example.org/list> "
                                   "<http://www.w3.org/1999/02/22-rdf-syntax-ns#nil>"));
}

TEST(turtle, base_resolves_relative_iris) {
    auto lines = parse_to_lines(
        "@base <http://example.org/dir/page> .\n"
        "<sub> <#pred> <../up> .\n");
    CHECK_EQ(lines.size(), std::size_t{1});
    CHECK_EQ(lines[0], std::string("<http://example.org/dir/sub> "
                                   "<http://example.org/dir/page#pred> <http://example.org/up>"));
}

TEST(turtle, triple_quoted_string_keeps_newlines) {
    auto lines = parse_to_lines(
        "@prefix ex: <http://example.org/> .\n"
        "ex:s ex:p \"\"\"line one\nline two\"\"\" .\n");
    CHECK_EQ(lines.size(), std::size_t{1});
    CHECK(contains(lines, "<http://example.org/s> <http://example.org/p> "
                          "\"line one\\nline two\""));
}

TEST(turtle, escapes_and_unicode) {
    auto lines = parse_to_lines(
        "@prefix ex: <http://example.org/> .\n"
        "ex:s ex:p \"quote \\\" tab \\t u \\u0041\" .\n");
    CHECK_EQ(lines.size(), std::size_t{1});
    CHECK(contains(lines, "<http://example.org/s> <http://example.org/p> "
                          "\"quote \\\" tab \\t u A\""));
}

TEST(turtle, comments_are_ignored) {
    auto lines = parse_to_lines(
        "# leading comment\n"
        "@prefix ex: <http://example.org/> . # after a directive\n"
        "ex:s ex:p ex:o . # after a statement\n");
    CHECK_EQ(lines.size(), std::size_t{1});
}

TEST(turtle, blank_scope_keeps_documents_apart) {
    TurtleOptions options;
    options.blank_scope = "doc1";
    auto lines = parse_to_lines("_:x <http://a/p> <http://a/o> .\n", options);
    CHECK_EQ(lines[0], std::string("_:doc1_x <http://a/p> <http://a/o>"));
}

TEST(turtle, undeclared_prefix_is_an_error) {
    CHECK_THROWS(parse_to_lines("ex:s ex:p ex:o .\n"), ParseError);
}

TEST(turtle, missing_full_stop_is_an_error) {
    CHECK_THROWS(parse_to_lines("<http://a/s> <http://a/p> <http://a/o>\n"), ParseError);
}

TEST(turtle, unterminated_literal_is_an_error) {
    CHECK_THROWS(parse_to_lines("<http://a/s> <http://a/p> \"open .\n"), ParseError);
}

TEST(turtle, error_carries_line_and_column) {
    bool caught = false;
    try {
        parse_to_lines(
            "<http://a/s> <http://a/p> <http://a/o> .\n"
            "<http://a/s> <http://a/p> ex:missing .\n");
    } catch (const ParseError& e) {
        caught = true;
        CHECK_EQ(e.line(), 2);
        CHECK(e.column() > 1);
    }
    CHECK(caught);
}

TEST(turtle, ntriples_mode_rejects_turtle_constructs) {
    TurtleOptions strict;
    strict.ntriples_only = true;
    // A plain N-Triples document is still accepted.
    CHECK_EQ(parse_to_lines("<http://a/s> <http://a/p> <http://a/o> .\n", strict).size(),
             std::size_t{1});
    CHECK_THROWS(parse_to_lines("@prefix ex: <http://example.org/> .\n", strict), ParseError);
    CHECK_THROWS(parse_to_lines("<http://a/s> a <http://a/o> .\n", strict), ParseError);
    CHECK_THROWS(parse_to_lines("<http://a/s> <http://a/p> 7 .\n", strict), ParseError);
    CHECK_THROWS(parse_to_lines("<http://a/s> <http://a/p> [ ] .\n", strict), ParseError);
}

TEST(turtle, load_into_store_counts_triples) {
    TripleStore store;
    std::size_t n = load_turtle(store,
                                "@prefix ex: <http://example.org/> .\n"
                                "ex:a ex:p ex:b, ex:c .\n"
                                "ex:a ex:p ex:b .\n");
    CHECK_EQ(n, std::size_t{3});
    store.build();
    // The duplicate is dropped by the index build, so the store holds two.
    CHECK_EQ(store.triple_count(), std::size_t{2});
}

TEST(turtle, failed_load_leaves_the_store_untouched) {
    TripleStore store;
    load_turtle(store, "<http://a/s> <http://a/p> <http://a/o> .\n");
    store.build();
    CHECK_EQ(store.triple_count(), std::size_t{1});
    CHECK_THROWS(load_turtle(store, "<http://a/s2> <http://a/p> <http://a/o2> .\n"
                                    "<http://a/broken> <http://a/p> ex:nope .\n"),
                 ParseError);
    store.build();
    CHECK_EQ(store.triple_count(), std::size_t{1});
}
