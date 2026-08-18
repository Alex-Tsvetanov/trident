// Every expected answer here was counted by hand from data/small.ttl.
#include "harness.hpp"

#include "fixture.hpp"

using namespace trident;
using fixture::ask;
using fixture::column;
using fixture::sorted_column;

TEST(exec, the_open_pattern_returns_every_triple) {
    TripleStore store = fixture::load_small();
    CHECK_EQ(ask(store, "SELECT * WHERE { ?s ?p ?o }").results.size(), std::size_t{40});
}

TEST(exec, single_pattern_with_a_constant_predicate) {
    TripleStore store = fixture::load_small();
    QueryOutcome out = ask(store, "SELECT ?n WHERE { ?p ex:name ?n }");
    CHECK_EQ(out.results.size(), std::size_t{4});
    std::vector<std::string> names = sorted_column(out, "n");
    CHECK_EQ(names[0], std::string("Alice"));
    CHECK_EQ(names[3], std::string("Dave"));
}

TEST(exec, star_join_on_a_shared_subject) {
    TripleStore store = fixture::load_small();
    QueryOutcome out = ask(store, "SELECT ?t WHERE { ?b ex:author ex:alice . ?b ex:title ?t }");
    std::vector<std::string> titles = sorted_column(out, "t");
    CHECK_EQ(titles.size(), std::size_t{2});
    CHECK_EQ(titles[0], std::string("Graphs"));
    CHECK_EQ(titles[1], std::string("Queries"));
}

TEST(exec, a_constant_in_the_object_position) {
    TripleStore store = fixture::load_small();
    QueryOutcome out = ask(store, "SELECT ?b WHERE { ?b ex:year 2021 }");
    CHECK_EQ(out.results.size(), std::size_t{2});
    std::vector<std::string> books = sorted_column(out, "b");
    CHECK_EQ(books[0], std::string("http://example.org/book2"));
    CHECK_EQ(books[1], std::string("http://example.org/book3"));
}

TEST(exec, a_two_step_path_through_the_graph) {
    TripleStore store = fixture::load_small();
    // alice knows bob, bob knows carol. That is the only chain of length two.
    QueryOutcome out = ask(store, "SELECT ?a ?b ?c WHERE { ?a ex:knows ?b . ?b ex:knows ?c }");
    CHECK_EQ(out.results.size(), std::size_t{1});
    CHECK_EQ(column(out, "a")[0], std::string("http://example.org/alice"));
    CHECK_EQ(column(out, "c")[0], std::string("http://example.org/carol"));
}

TEST(exec, a_variable_repeated_in_one_pattern_must_match_itself) {
    TripleStore store = fixture::load_small();
    CHECK_EQ(ask(store, "SELECT ?x WHERE { ?x ex:knows ?x }").results.size(), std::size_t{0});
    CHECK_EQ(ask(store, "SELECT ?x WHERE { ?x ?p ?x }").results.size(), std::size_t{0});
}

TEST(exec, an_unknown_constant_yields_no_solutions) {
    TripleStore store = fixture::load_small();
    CHECK_EQ(ask(store, "SELECT ?o WHERE { ex:nobody ex:name ?o }").results.size(),
             std::size_t{0});
    CHECK_EQ(ask(store, "SELECT ?s WHERE { ?s ex:noSuchPredicate ?o }").results.size(),
             std::size_t{0});
}

TEST(exec, filter_on_a_numeric_literal) {
    TripleStore store = fixture::load_small();
    QueryOutcome out = ask(store, "SELECT ?n WHERE { ?p ex:name ?n . ?p ex:age ?a . "
                                  "FILTER (?a > 35) }");
    std::vector<std::string> names = sorted_column(out, "n");
    CHECK_EQ(names.size(), std::size_t{2});
    CHECK_EQ(names[0], std::string("Bob"));
    CHECK_EQ(names[1], std::string("Dave"));
}

TEST(exec, filter_with_and_or_and_negation) {
    TripleStore store = fixture::load_small();
    CHECK_EQ(ask(store, "SELECT ?p WHERE { ?p ex:age ?a FILTER (?a > 30 && ?a < 45) }")
                 .results.size(),
             std::size_t{2});  // alice 34, bob 41
    CHECK_EQ(ask(store, "SELECT ?p WHERE { ?p ex:age ?a FILTER (?a < 30 || ?a > 50) }")
                 .results.size(),
             std::size_t{2});  // carol 29, dave 52
    CHECK_EQ(ask(store, "SELECT ?p WHERE { ?p ex:age ?a FILTER (!(?a > 30)) }").results.size(),
             std::size_t{1});  // carol 29
}

TEST(exec, filter_with_string_functions) {
    TripleStore store = fixture::load_small();
    CHECK_EQ(ask(store, "SELECT ?n WHERE { ?p ex:name ?n FILTER STRSTARTS(?n, \"A\") }")
                 .results.size(),
             std::size_t{1});
    CHECK_EQ(ask(store, "SELECT ?n WHERE { ?p ex:name ?n FILTER (STRLEN(?n) = 5) }")
                 .results.size(),
             std::size_t{2});  // Alice, Carol
    CHECK_EQ(ask(store, "SELECT ?n WHERE { ?p ex:name ?n FILTER REGEX(?n, \"^[bd]\", \"i\") }")
                 .results.size(),
             std::size_t{2});  // Bob, Dave
}

TEST(exec, filter_on_term_kind_and_language) {
    TripleStore store = fixture::load_small();
    // 4 names, 4 ages, 4 titles, 4 years, 4 page counts
    CHECK_EQ(ask(store, "SELECT ?o WHERE { ?s ?p ?o FILTER isLiteral(?o) }").results.size(),
             std::size_t{20});
    CHECK_EQ(ask(store, "SELECT ?t WHERE { ?b ex:title ?t FILTER (LANG(?t) = \"en\") }")
                 .results.size(),
             std::size_t{4});
    CHECK_EQ(ask(store, "SELECT ?s WHERE { ?s ?p ?o FILTER isBlank(?s) }").results.size(),
             std::size_t{0});
}

TEST(exec, optional_keeps_rows_without_a_match) {
    TripleStore store = fixture::load_small();
    QueryOutcome out = ask(store, "SELECT ?n ?h WHERE { ?p a ex:Author . ?p ex:name ?n "
                                  "OPTIONAL { ?p ex:homepage ?h } }");
    CHECK_EQ(out.results.size(), std::size_t{3});
    std::vector<std::string> homepages = sorted_column(out, "h");
    CHECK_EQ(homepages[0], std::string("<unbound>"));
    CHECK_EQ(homepages[1], std::string("<unbound>"));
    CHECK_EQ(homepages[2], std::string("http://example.org/people/alice"));
}

TEST(exec, bound_tells_a_missing_optional_apart) {
    TripleStore store = fixture::load_small();
    CHECK_EQ(ask(store, "SELECT ?n WHERE { ?p a ex:Author . ?p ex:name ?n "
                        "OPTIONAL { ?p ex:homepage ?h } FILTER (!BOUND(?h)) }")
                 .results.size(),
             std::size_t{2});
}

TEST(exec, union_takes_both_alternatives) {
    TripleStore store = fixture::load_small();
    QueryOutcome out = ask(store, "SELECT ?s WHERE { { ?s a ex:Author } UNION { ?s a ex:Reader } }");
    CHECK_EQ(out.results.size(), std::size_t{4});
}

TEST(exec, distinct_removes_repeated_solutions) {
    TripleStore store = fixture::load_small();
    CHECK_EQ(ask(store, "SELECT ?y WHERE { ?b ex:year ?y }").results.size(), std::size_t{4});
    CHECK_EQ(ask(store, "SELECT DISTINCT ?y WHERE { ?b ex:year ?y }").results.size(),
             std::size_t{3});
}

TEST(exec, order_by_sorts_numerically_not_lexically) {
    TripleStore store = fixture::load_small();
    QueryOutcome out = ask(store, "SELECT ?t WHERE { ?b ex:title ?t . ?b ex:pages ?n } "
                                  "ORDER BY DESC(?n)");
    std::vector<std::string> titles = column(out, "t");
    CHECK_EQ(titles.size(), std::size_t{4});
    CHECK_EQ(titles[0], std::string("Queries"));   // 340
    CHECK_EQ(titles[1], std::string("Schemas"));   // 275
    CHECK_EQ(titles[2], std::string("Graphs"));    // 210
    CHECK_EQ(titles[3], std::string("Indexes"));   // 150
}

TEST(exec, limit_and_offset_cut_the_sequence) {
    TripleStore store = fixture::load_small();
    QueryOutcome out = ask(store, "SELECT ?t WHERE { ?b ex:title ?t . ?b ex:pages ?n } "
                                  "ORDER BY ?n LIMIT 2 OFFSET 1");
    std::vector<std::string> titles = column(out, "t");
    CHECK_EQ(titles.size(), std::size_t{2});
    CHECK_EQ(titles[0], std::string("Graphs"));   // 210, second smallest
    CHECK_EQ(titles[1], std::string("Schemas"));  // 275
}

TEST(exec, a_projected_variable_the_pattern_never_binds_is_a_column_of_unbound) {
    TripleStore store = fixture::load_small();
    QueryOutcome out = ask(store, "SELECT ?n ?missing WHERE { ?p ex:name ?n } LIMIT 1");
    CHECK_EQ(out.results.columns.size(), std::size_t{2});
    CHECK_EQ(column(out, "missing")[0], std::string("<unbound>"));
}

TEST(exec, the_planner_does_not_change_the_answers) {
    TripleStore store = fixture::load_small();
    const char* body =
        "SELECT ?n ?t WHERE { ?b ex:title ?t . ?b ex:author ?p . ?p ex:name ?n . "
        "?b ex:year 2021 }";
    PlanOptions planned, naive, no_merge;
    naive.naive_join_order = true;
    no_merge.enable_merge_join = false;
    std::vector<std::string> a = sorted_column(ask(store, body, planned), "t");
    std::vector<std::string> b = sorted_column(ask(store, body, naive), "t");
    std::vector<std::string> c = sorted_column(ask(store, body, no_merge), "t");
    CHECK_EQ(a.size(), std::size_t{2});
    CHECK(a == b);
    CHECK(a == c);
}

TEST(exec, counting_and_decoding_agree) {
    TripleStore store = fixture::load_small();
    std::string text = std::string(fixture::prefixes()) +
                       "SELECT ?s ?o WHERE { ?s ex:knows ?o }";
    CHECK_EQ(count_query(store, text), std::size_t{3});
    CHECK_EQ(run_query(store, text).results.size(), std::size_t{3});
}
