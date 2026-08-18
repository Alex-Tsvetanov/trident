// The demo runs over a generated dataset, so the expected answers here are
// derived from the generator's contract rather than counted off a fixed file.
// The generator gives every paper exactly one type, title, year, venue, topic,
// citation count and main author, and every author exactly one type, name,
// affiliation and h-index. Every assertion below follows from that.
#include "harness.hpp"

#include <set>
#include <string>

#include "fixture.hpp"
#include "trident/generator.hpp"
#include "trident/rdfs.hpp"

using namespace trident;

namespace {

constexpr std::size_t kPapers = 200;
constexpr std::size_t kAuthors = 50;
constexpr std::size_t kVenues = 8;
constexpr std::size_t kTopics = 12;
constexpr std::size_t kOrganisations = 10;

DatasetSpec spec() {
    DatasetSpec out;
    out.papers = kPapers;
    out.authors = kAuthors;
    out.venues = kVenues;
    out.topics = kTopics;
    out.organisations = kOrganisations;
    return out;
}

TripleStore load_generated() {
    TripleStore store;
    load_turtle(store, generate_dataset(spec()));
    store.build();
    return store;
}

long long as_number(const QueryOutcome& outcome, const std::string& name) {
    std::vector<std::string> values = fixture::column(outcome, name);
    return values.empty() ? -1 : std::stoll(values[0]);
}

}  // namespace

TEST(demo_queries, the_generator_is_deterministic) {
    CHECK_EQ(generate_dataset(spec()), generate_dataset(spec()));
    DatasetSpec other = spec();
    other.seed += 1;
    CHECK(generate_dataset(spec()) != generate_dataset(other));
}

TEST(demo_queries, every_paper_has_exactly_one_of_each_single_valued_property) {
    TripleStore store = load_generated();
    for (const char* predicate : {"a ex:Paper", "ex:title ?v", "ex:year ?v", "ex:venue ?v",
                                  "ex:topic ?v", "ex:citationCount ?v", "ex:mainAuthor ?v"}) {
        QueryOutcome out =
            fixture::ask(store, std::string("SELECT (COUNT(*) AS ?n) WHERE { ?p ") +
                                    predicate + " }");
        CHECK_EQ(as_number(out, "n"), static_cast<long long>(kPapers));
    }
}

TEST(demo_queries, the_class_counts_match_the_specification) {
    TripleStore store = load_generated();
    struct Expectation { const char* type; std::size_t count; };
    const Expectation expectations[] = {{"ex:Paper", kPapers},
                                        {"ex:Author", kAuthors},
                                        {"ex:Venue", kVenues},
                                        {"ex:Topic", kTopics},
                                        {"ex:Organisation", kOrganisations}};
    for (const Expectation& item : expectations) {
        QueryOutcome out = fixture::ask(
            store, std::string("SELECT (COUNT(?x) AS ?n) WHERE { ?x a ") + item.type + " }");
        CHECK_EQ(as_number(out, "n"), static_cast<long long>(item.count));
    }
}

TEST(demo_queries, grouping_by_venue_partitions_the_papers) {
    TripleStore store = load_generated();
    QueryOutcome out = fixture::ask(store, "SELECT ?v (COUNT(?p) AS ?n) WHERE "
                                           "{ ?p ex:venue ?v } GROUP BY ?v");
    CHECK_EQ(out.results.size(), kVenues);
    long long total = 0;
    for (const std::string& value : fixture::column(out, "n")) total += std::stoll(value);
    CHECK_EQ(total, static_cast<long long>(kPapers));
}

TEST(demo_queries, optional_does_not_drop_authors_without_a_homepage) {
    TripleStore store = load_generated();
    QueryOutcome rows = fixture::ask(store, "SELECT ?n ?h WHERE { ?a a ex:Author . "
                                            "?a ex:name ?n OPTIONAL { ?a ex:homepage ?h } }");
    CHECK_EQ(rows.results.size(), kAuthors);
    QueryOutcome counted =
        fixture::ask(store, "SELECT (COUNT(?a) AS ?all) (COUNT(?h) AS ?with) WHERE "
                            "{ ?a a ex:Author OPTIONAL { ?a ex:homepage ?h } }");
    CHECK_EQ(as_number(counted, "all"), static_cast<long long>(kAuthors));
    CHECK(as_number(counted, "with") > 0);
    CHECK(as_number(counted, "with") < static_cast<long long>(kAuthors));
}

TEST(demo_queries, union_covers_both_branches_without_overlap) {
    TripleStore store = load_generated();
    QueryOutcome out = fixture::ask(store, "SELECT DISTINCT ?x WHERE "
                                           "{ { ?x a ex:Venue } UNION { ?x a ex:Topic } }");
    CHECK_EQ(out.results.size(), kVenues + kTopics);
}

TEST(demo_queries, years_stay_inside_the_generated_range) {
    TripleStore store = load_generated();
    QueryOutcome out = fixture::ask(store, "SELECT DISTINCT ?y WHERE { ?p ex:year ?y } "
                                           "ORDER BY ?y");
    std::vector<std::string> years = fixture::column(out, "y");
    CHECK(!years.empty());
    CHECK(std::stoll(years.front()) >= 2010);
    CHECK(std::stoll(years.back()) <= 2025);
    for (std::size_t i = 1; i < years.size(); ++i) {
        CHECK(std::stoll(years[i - 1]) < std::stoll(years[i]));
    }
}

TEST(demo_queries, the_planner_never_changes_the_solutions) {
    TripleStore store = load_generated();
    const char* body =
        "SELECT ?title ?name WHERE {\n"
        "  ?paper ex:title ?title .\n"
        "  ?paper ex:mainAuthor ?a .\n"
        "  ?a ex:name ?name .\n"
        "  ?a ex:affiliation ex:org3 .\n"
        "  ?paper ex:venue ex:venue2 .\n"
        "}";
    PlanOptions planned;
    PlanOptions naive;
    naive.naive_join_order = true;
    PlanOptions no_merge;
    no_merge.enable_merge_join = false;

    QueryOutcome a = fixture::ask(store, body, planned);
    QueryOutcome b = fixture::ask(store, body, naive);
    QueryOutcome c = fixture::ask(store, body, no_merge);
    CHECK_EQ(a.results.size(), b.results.size());
    CHECK_EQ(a.results.size(), c.results.size());
    CHECK_EQ(fixture::sorted_column(a, "title"), fixture::sorted_column(b, "title"));
    CHECK_EQ(fixture::sorted_column(a, "title"), fixture::sorted_column(c, "title"));
    // The planned order really is different from the written one: the written
    // order starts with the least selective pattern.
    CHECK(a.plan_text != b.plan_text);
}

TEST(demo_queries, merge_join_and_nested_loop_join_agree_on_the_generated_data) {
    TripleStore store = load_generated();
    const char* body = "SELECT ?a ?first ?second WHERE "
                       "{ ?first ex:mainAuthor ?a . ?second ex:author ?a }";
    PlanOptions with_merge;
    PlanOptions without_merge;
    without_merge.enable_merge_join = false;
    QueryOutcome merged = fixture::ask(store, body, with_merge);
    QueryOutcome looped = fixture::ask(store, body, without_merge);
    CHECK(merged.plan_text.find("MergeJoin") != std::string::npos);
    CHECK(looped.plan_text.find("MergeJoin") == std::string::npos);
    CHECK_EQ(merged.results.size(), looped.results.size());
    CHECK(merged.results.size() > 0);

    std::multiset<std::string> left, right;
    for (const auto& row : merged.results.rows) {
        left.insert(row[0].value + "|" + row[1].value + "|" + row[2].value);
    }
    for (const auto& row : looped.results.rows) {
        right.insert(row[0].value + "|" + row[1].value + "|" + row[2].value);
    }
    CHECK(left == right);
}

TEST(demo_queries, limit_returns_early_without_changing_the_prefix) {
    TripleStore store = load_generated();
    QueryOutcome all = fixture::ask(store, "SELECT ?p ?t WHERE { ?p ex:title ?t } "
                                           "ORDER BY ?p");
    QueryOutcome few = fixture::ask(store, "SELECT ?p ?t WHERE { ?p ex:title ?t } "
                                           "ORDER BY ?p LIMIT 5");
    CHECK_EQ(all.results.size(), kPapers);
    CHECK_EQ(few.results.size(), std::size_t{5});
    for (std::size_t i = 0; i < 5; ++i) {
        CHECK_EQ(all.results.rows[i][0].value, few.results.rows[i][0].value);
    }
}

TEST(demo_queries, rdfs_inference_reaches_the_expected_totals) {
    TripleStore store = load_generated();
    CHECK_EQ(fixture::ask(store, "SELECT ?x WHERE { ?x a ex:Agent }").results.size(),
             std::size_t{0});
    materialise_rdfs(store);
    // Author and Organisation are the only subclasses of Agent in the generated
    // schema, through Person for the authors.
    QueryOutcome agents = fixture::ask(store, "SELECT (COUNT(?x) AS ?n) WHERE { ?x a ex:Agent }");
    CHECK_EQ(as_number(agents, "n"), static_cast<long long>(kAuthors + kOrganisations));
    // ex:mainAuthor is a subproperty of ex:author, whose domain is ex:Publication
    // and ex:Publication is a subclass of ex:Work, so every paper is a Work.
    QueryOutcome works = fixture::ask(store, "SELECT (COUNT(?x) AS ?n) WHERE { ?x a ex:Work }");
    CHECK_EQ(as_number(works, "n"), static_cast<long long>(kPapers));
}
