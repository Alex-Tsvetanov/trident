#include "harness.hpp"

#include "fixture.hpp"
#include "trident/query.hpp"
#include "trident/rdfs.hpp"
#include "trident/turtle.hpp"

using namespace trident;

TEST(rdfs_query, subclass_answers_match_materialisation) {
    TripleStore store = fixture::load_small();
    PlanOptions qt;
    qt.rdfs_query_time = true;
    CHECK_EQ(fixture::ask(store, "SELECT ?x WHERE { ?x a ex:Person }", qt).results.size(),
             std::size_t{4});

    TripleStore materialised = fixture::load_small();
    materialise_rdfs(materialised);
    CHECK_EQ(fixture::ask(materialised, "SELECT ?x WHERE { ?x a ex:Person }").results.size(),
             std::size_t{4});
    // Query-time mode leaves the store untouched.
    CHECK_EQ(store.triple_count(), std::size_t{40});
}

TEST(rdfs_query, subproperty_is_expanded_without_writing_triples) {
    TripleStore store;
    load_turtle(store,
                "@prefix ex: <http://example.org/> .\n"
                "@prefix rdfs: <http://www.w3.org/2000/01/rdf-schema#> .\n"
                "ex:mainAuthor rdfs:subPropertyOf ex:author .\n"
                "ex:author rdfs:subPropertyOf ex:contributor .\n"
                "ex:b1 ex:mainAuthor ex:p1 .\n");
    store.build();
    PlanOptions qt;
    qt.rdfs_query_time = true;
    CHECK_EQ(fixture::ask(store, "SELECT ?p WHERE { ex:b1 ex:author ?p }", qt).results.size(),
             std::size_t{1});
    CHECK_EQ(
        fixture::ask(store, "SELECT ?p WHERE { ex:b1 ex:contributor ?p }", qt).results.size(),
        std::size_t{1});
    CHECK_EQ(store.triple_count(), std::size_t{3});
}

TEST(rdfs_query, domain_and_range_infer_types_at_query_time) {
    TripleStore store;
    load_turtle(store,
                "@prefix ex: <http://example.org/> .\n"
                "@prefix rdfs: <http://www.w3.org/2000/01/rdf-schema#> .\n"
                "ex:wrote rdfs:domain ex:Author . ex:wrote rdfs:range ex:Book .\n"
                "ex:p1 ex:wrote ex:b1 .\n");
    store.build();
    PlanOptions qt;
    qt.rdfs_query_time = true;
    CHECK_EQ(fixture::ask(store, "SELECT ?x WHERE { ?x a ex:Author }", qt).results.size(),
             std::size_t{1});
    CHECK_EQ(fixture::ask(store, "SELECT ?x WHERE { ?x a ex:Book }", qt).results.size(),
             std::size_t{1});
}

TEST(rdfs_query, range_then_subclass_chain_matches_materialisation) {
    TripleStore store;
    load_turtle(store,
                "@prefix ex: <http://example.org/> .\n"
                "@prefix rdfs: <http://www.w3.org/2000/01/rdf-schema#> .\n"
                "ex:wrote rdfs:range ex:Person . ex:Person rdfs:subClassOf ex:Agent .\n"
                "ex:b1 ex:wrote ex:p1 .\n");
    store.build();
    PlanOptions qt;
    qt.rdfs_query_time = true;
    CHECK_EQ(fixture::ask(store, "SELECT ?x WHERE { ?x a ex:Agent }", qt).results.size(),
             std::size_t{1});
}
