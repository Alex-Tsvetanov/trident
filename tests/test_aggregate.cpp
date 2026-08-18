// Aggregates over data/small.ttl. The four books have 210, 340, 150 and 275
// pages, so the sum is 975 and the average 243.75.
#include "harness.hpp"

#include "fixture.hpp"

using namespace trident;
using fixture::ask;
using fixture::column;
using fixture::sorted_column;

TEST(aggregate, count_over_the_whole_graph) {
    TripleStore store = fixture::load_small();
    QueryOutcome out = ask(store, "SELECT (COUNT(*) AS ?n) WHERE { ?s ?p ?o }");
    CHECK_EQ(out.results.size(), std::size_t{1});
    CHECK_EQ(column(out, "n")[0], std::string("40"));
}

TEST(aggregate, count_over_an_empty_result_is_zero_and_still_one_row) {
    TripleStore store = fixture::load_small();
    QueryOutcome out = ask(store, "SELECT (COUNT(*) AS ?n) WHERE { ?s ex:noSuchPredicate ?o }");
    CHECK_EQ(out.results.size(), std::size_t{1});
    CHECK_EQ(column(out, "n")[0], std::string("0"));
}

TEST(aggregate, count_distinct_differs_from_count) {
    TripleStore store = fixture::load_small();
    QueryOutcome out = ask(store, "SELECT (COUNT(?y) AS ?n) (COUNT(DISTINCT ?y) AS ?d) "
                                  "WHERE { ?b ex:year ?y }");
    CHECK_EQ(column(out, "n")[0], std::string("4"));
    CHECK_EQ(column(out, "d")[0], std::string("3"));
}

TEST(aggregate, sum_min_max_and_average_over_page_counts) {
    TripleStore store = fixture::load_small();
    QueryOutcome out = ask(store, "SELECT (SUM(?n) AS ?total) (MIN(?n) AS ?least) "
                                  "(MAX(?n) AS ?most) (AVG(?n) AS ?mean) "
                                  "WHERE { ?b ex:pages ?n }");
    CHECK_EQ(column(out, "total")[0], std::string("975"));
    CHECK_EQ(column(out, "least")[0], std::string("150"));
    CHECK_EQ(column(out, "most")[0], std::string("340"));
    CHECK_EQ(column(out, "mean")[0], std::string("243.75"));
}

TEST(aggregate, group_by_splits_the_input) {
    TripleStore store = fixture::load_small();
    QueryOutcome out = ask(store, "SELECT ?a (COUNT(?b) AS ?n) WHERE { ?b ex:author ?a } "
                                  "GROUP BY ?a ORDER BY ?a");
    CHECK_EQ(out.results.size(), std::size_t{3});
    std::vector<std::string> authors = column(out, "a");
    std::vector<std::string> counts = column(out, "n");
    CHECK_EQ(authors[0], std::string("http://example.org/alice"));
    CHECK_EQ(counts[0], std::string("2"));
    CHECK_EQ(authors[1], std::string("http://example.org/bob"));
    CHECK_EQ(counts[1], std::string("1"));
    CHECK_EQ(authors[2], std::string("http://example.org/carol"));
    CHECK_EQ(counts[2], std::string("1"));
}

TEST(aggregate, ordering_by_an_aggregate_result) {
    TripleStore store = fixture::load_small();
    QueryOutcome out = ask(store, "SELECT ?a (COUNT(?b) AS ?n) WHERE { ?b ex:author ?a } "
                                  "GROUP BY ?a ORDER BY DESC(?n) LIMIT 1");
    CHECK_EQ(out.results.size(), std::size_t{1});
    CHECK_EQ(column(out, "a")[0], std::string("http://example.org/alice"));
    CHECK_EQ(column(out, "n")[0], std::string("2"));
}

TEST(aggregate, group_by_a_variable_from_a_join) {
    TripleStore store = fixture::load_small();
    // Books per publication year, with the year taken from a second pattern.
    QueryOutcome out = ask(store, "SELECT ?y (COUNT(?b) AS ?n) WHERE "
                                  "{ ?b a ex:Book . ?b ex:year ?y } GROUP BY ?y ORDER BY ?y");
    CHECK_EQ(out.results.size(), std::size_t{3});
    CHECK_EQ(column(out, "y")[0], std::string("2019"));
    CHECK_EQ(column(out, "n")[0], std::string("1"));
    CHECK_EQ(column(out, "y")[1], std::string("2021"));
    CHECK_EQ(column(out, "n")[1], std::string("2"));
    CHECK_EQ(column(out, "y")[2], std::string("2023"));
    CHECK_EQ(column(out, "n")[2], std::string("1"));
}

TEST(aggregate, min_and_max_over_a_string_column) {
    TripleStore store = fixture::load_small();
    QueryOutcome out = ask(store, "SELECT (MIN(?n) AS ?first) (MAX(?n) AS ?last) "
                                  "WHERE { ?p ex:name ?n }");
    CHECK_EQ(column(out, "first")[0], std::string("Alice"));
    CHECK_EQ(column(out, "last")[0], std::string("Dave"));
}

TEST(aggregate, an_aggregate_ignores_unbound_values) {
    TripleStore store = fixture::load_small();
    // Only alice has a homepage, so counting the optional variable gives one even
    // though the group has three authors.
    QueryOutcome out = ask(store, "SELECT (COUNT(?p) AS ?authors) (COUNT(?h) AS ?homepages) "
                                  "WHERE { ?p a ex:Author OPTIONAL { ?p ex:homepage ?h } }");
    CHECK_EQ(column(out, "authors")[0], std::string("3"));
    CHECK_EQ(column(out, "homepages")[0], std::string("1"));
}
