// The planner: which index a pattern reaches for, in what order the joins run,
// and when a merge join is legal.
#include "harness.hpp"

#include <string>

#include "fixture.hpp"
#include "trident/planner.hpp"

using namespace trident;

namespace {

std::string plan_of(TripleStore& store, const std::string& body,
                    const PlanOptions& options = {}) {
    Query query = parse_sparql(fixture::prefixes() + body);
    Plan plan = build_plan(store, query, options);
    return plan.text;
}

bool has(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

std::size_t position(const std::string& haystack, const std::string& needle) {
    std::size_t at = haystack.find(needle);
    return at == std::string::npos ? haystack.size() + 1 : at;
}

}  // namespace

TEST(planner, index_choice_follows_the_bound_positions) {
    TripleStore store = fixture::load_small();
    CHECK(has(plan_of(store, "SELECT * WHERE { ex:alice ex:name ?n }"), "IndexScan SPO prefix=2"));
    CHECK(has(plan_of(store, "SELECT * WHERE { ?s ex:name ?n }"), "IndexScan POS prefix=1"));
    CHECK(has(plan_of(store, "SELECT * WHERE { ?s ?p ex:carol }"), "IndexScan OSP prefix=1"));
    CHECK(has(plan_of(store, "SELECT * WHERE { ?s ?p ?o }"), "IndexScan SPO prefix=0"));
    CHECK(has(plan_of(store, "SELECT * WHERE { ex:alice ?p ex:bob }"), "IndexScan OSP prefix=2"));
}

TEST(planner, cardinalities_in_the_plan_are_exact_counts) {
    TripleStore store = fixture::load_small();
    // Four names, six triples about alice, forty triples in total.
    CHECK(has(plan_of(store, "SELECT * WHERE { ?s ex:name ?n }"), "card=4"));
    CHECK(has(plan_of(store, "SELECT * WHERE { ex:alice ?p ?o }"), "card=6"));
    CHECK(has(plan_of(store, "SELECT * WHERE { ?s ?p ?o }"), "card=40"));
}

TEST(planner, the_most_selective_pattern_is_scanned_first) {
    TripleStore store = fixture::load_small();
    // ex:year 2021 matches two triples, ?b ex:title ?t matches four. The selective
    // one has to be the outer scan whichever order the query is written in.
    std::string forward =
        plan_of(store, "SELECT * WHERE { ?b ex:title ?t . ?b ex:year 2021 }");
    std::string reverse =
        plan_of(store, "SELECT * WHERE { ?b ex:year 2021 . ?b ex:title ?t }");
    CHECK(position(forward, ":year") < position(forward, ":title"));
    CHECK(position(reverse, ":year") < position(reverse, ":title"));
}

TEST(planner, the_naive_flag_keeps_the_written_order) {
    TripleStore store = fixture::load_small();
    PlanOptions naive;
    naive.naive_join_order = true;
    std::string plan = plan_of(store, "SELECT * WHERE { ?b ex:title ?t . ?b ex:year 2021 }", naive);
    CHECK(position(plan, ":title") < position(plan, ":year"));
}

TEST(planner, an_impossible_pattern_is_recognised_without_a_scan) {
    TripleStore store = fixture::load_small();
    std::string plan = plan_of(store, "SELECT * WHERE { ?s ex:noSuchPredicate ?o }");
    CHECK(has(plan, "card=0"));
}

TEST(planner, the_inner_scan_uses_the_bindings_of_the_outer_one) {
    TripleStore store = fixture::load_small();
    // ?b is bound by the first scan, so the second reaches for SPO with a prefix
    // of two rather than scanning POS and filtering.
    std::string plan = plan_of(store, "SELECT * WHERE { ?b ex:year 2021 . ?b ex:title ?t }");
    CHECK(has(plan, "IndexNestedLoopJoin"));
    CHECK(has(plan, "IndexScan SPO prefix=2 card=4  { ?b :title ?t }"));
}

TEST(planner, a_join_on_the_object_position_becomes_a_merge_join) {
    TripleStore store = fixture::load_small();
    // Both scans have a bound predicate, so both are read from POS in object
    // order, and the shared variable is that object.
    std::string plan = plan_of(store, "SELECT * WHERE { ?x ex:knows ?p . ?y ex:author ?p }");
    CHECK(has(plan, "MergeJoin"));
}

TEST(planner, merge_join_can_be_switched_off) {
    TripleStore store = fixture::load_small();
    PlanOptions no_merge;
    no_merge.enable_merge_join = false;
    std::string plan =
        plan_of(store, "SELECT * WHERE { ?x ex:knows ?p . ?y ex:author ?p }", no_merge);
    CHECK(!has(plan, "MergeJoin"));
    CHECK(has(plan, "IndexNestedLoopJoin"));
}

TEST(planner, merge_join_and_nested_loop_join_agree_on_the_answer) {
    TripleStore store = fixture::load_small();
    const char* body = "SELECT ?x ?y ?p WHERE { ?x ex:knows ?p . ?y ex:author ?p }";
    PlanOptions with_merge, without_merge;
    without_merge.enable_merge_join = false;
    QueryOutcome a = fixture::ask(store, body, with_merge);
    QueryOutcome b = fixture::ask(store, body, without_merge);
    CHECK(a.plan_text.find("MergeJoin") != std::string::npos);
    CHECK_EQ(a.results.size(), b.results.size());
    CHECK_EQ(fixture::sorted_column(a, "x"), fixture::sorted_column(b, "x"));
    CHECK_EQ(fixture::sorted_column(a, "y"), fixture::sorted_column(b, "y"));
}

TEST(planner, variable_slots_are_shared_across_the_whole_query) {
    TripleStore store = fixture::load_small();
    Query query = parse_sparql(std::string(fixture::prefixes()) +
                               "SELECT ?n WHERE { ?p ex:name ?n . ?p ex:age ?a } ORDER BY ?a");
    Plan plan = build_plan(store, query);
    CHECK_EQ(plan.variables.size(), std::size_t{3});  // p, n, a
    CHECK_EQ(plan.columns.size(), std::size_t{1});
    CHECK_EQ(plan.column_slots.size(), std::size_t{1});
    CHECK(plan.column_slots[0] >= 0);
}

TEST(planner, an_empty_group_pattern_plans_to_a_single_row) {
    TripleStore store = fixture::load_small();
    std::string plan = plan_of(store, "SELECT * WHERE { }");
    CHECK(has(plan, "Unit"));
    CHECK_EQ(fixture::ask(store, "SELECT * WHERE { }").results.size(), std::size_t{1});
}
