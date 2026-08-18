// RDFS materialisation over data/small.ttl.
//
// The schema there is: ex:Author and ex:Reader are subclasses of ex:Person,
// ex:author has domain ex:Book and range ex:Person. Alice, Bob and Carol are
// Authors and Dave is a Reader, so exactly four new triples follow: one
// "type Person" for each of them. The domain rule adds nothing, because all four
// books are already typed ex:Book.
#include "harness.hpp"

#include "fixture.hpp"
#include "trident/rdfs.hpp"

using namespace trident;
using fixture::ask;

TEST(rdfs, nothing_is_inferred_before_the_pass_runs) {
    TripleStore store = fixture::load_small();
    CHECK_EQ(ask(store, "SELECT ?x WHERE { ?x a ex:Person }").results.size(), std::size_t{0});
}

TEST(rdfs, subclass_gives_every_author_and_reader_a_person_type) {
    TripleStore store = fixture::load_small();
    RdfsStats stats = materialise_rdfs(store);
    CHECK_EQ(stats.before, std::size_t{40});
    CHECK_EQ(stats.inferred, std::size_t{4});
    CHECK_EQ(stats.after, std::size_t{44});
    CHECK_EQ(ask(store, "SELECT ?x WHERE { ?x a ex:Person }").results.size(), std::size_t{4});
}

TEST(rdfs, the_pass_is_idempotent) {
    TripleStore store = fixture::load_small();
    materialise_rdfs(store);
    RdfsStats second = materialise_rdfs(store);
    CHECK_EQ(second.inferred, std::size_t{0});
    CHECK_EQ(second.after, std::size_t{44});
}

TEST(rdfs, subclass_chains_are_closed_transitively) {
    TripleStore store;
    load_turtle(store,
                "@prefix ex: <http://example.org/> .\n"
                "@prefix rdfs: <http://www.w3.org/2000/01/rdf-schema#> .\n"
                "ex:A rdfs:subClassOf ex:B . ex:B rdfs:subClassOf ex:C . "
                "ex:C rdfs:subClassOf ex:D .\n"
                "ex:x a ex:A .\n");
    store.build();
    materialise_rdfs(store);
    // x gains B, C and D; A gains C and D; B gains D.
    CHECK_EQ(ask(store, "SELECT ?t WHERE { ex:x a ?t }").results.size(), std::size_t{4});
    CHECK_EQ(ask(store, "SELECT ?c WHERE { ex:A rdfs:subClassOf ?c }").results.size(),
             std::size_t{3});
}

TEST(rdfs, subproperty_copies_triples_onto_the_superproperty) {
    TripleStore store;
    load_turtle(store,
                "@prefix ex: <http://example.org/> .\n"
                "@prefix rdfs: <http://www.w3.org/2000/01/rdf-schema#> .\n"
                "ex:mainAuthor rdfs:subPropertyOf ex:author .\n"
                "ex:author rdfs:subPropertyOf ex:contributor .\n"
                "ex:b1 ex:mainAuthor ex:p1 .\n");
    store.build();
    materialise_rdfs(store);
    CHECK_EQ(ask(store, "SELECT ?p WHERE { ex:b1 ex:author ?p }").results.size(), std::size_t{1});
    CHECK_EQ(ask(store, "SELECT ?p WHERE { ex:b1 ex:contributor ?p }").results.size(),
             std::size_t{1});
}

TEST(rdfs, domain_and_range_give_types_to_subject_and_object) {
    TripleStore store;
    load_turtle(store,
                "@prefix ex: <http://example.org/> .\n"
                "@prefix rdfs: <http://www.w3.org/2000/01/rdf-schema#> .\n"
                "ex:wrote rdfs:domain ex:Author . ex:wrote rdfs:range ex:Book .\n"
                "ex:p1 ex:wrote ex:b1 .\n");
    store.build();
    RdfsStats stats = materialise_rdfs(store);
    CHECK_EQ(stats.inferred, std::size_t{2});
    CHECK_EQ(ask(store, "SELECT ?x WHERE { ?x a ex:Author }").results.size(), std::size_t{1});
    CHECK_EQ(ask(store, "SELECT ?x WHERE { ?x a ex:Book }").results.size(), std::size_t{1});
}

TEST(rdfs, a_range_type_is_then_widened_by_subclass) {
    // Two rules have to fire in sequence: range gives ex:p1 the type ex:Person,
    // and subclass then gives it ex:Agent. This is why the pass iterates.
    TripleStore store;
    load_turtle(store,
                "@prefix ex: <http://example.org/> .\n"
                "@prefix rdfs: <http://www.w3.org/2000/01/rdf-schema#> .\n"
                "ex:wrote rdfs:range ex:Person . ex:Person rdfs:subClassOf ex:Agent .\n"
                "ex:b1 ex:wrote ex:p1 .\n");
    store.build();
    RdfsStats stats = materialise_rdfs(store);
    CHECK(stats.rounds >= 2);
    CHECK_EQ(ask(store, "SELECT ?x WHERE { ?x a ex:Agent }").results.size(), std::size_t{1});
}

TEST(rdfs, a_graph_without_a_schema_gains_nothing) {
    TripleStore store;
    load_turtle(store, "<http://a/s> <http://a/p> <http://a/o> .\n");
    store.build();
    RdfsStats stats = materialise_rdfs(store);
    CHECK_EQ(stats.inferred, std::size_t{0});
    CHECK_EQ(stats.after, std::size_t{1});
}

TEST(rdfs, a_cycle_in_the_class_hierarchy_still_terminates) {
    TripleStore store;
    load_turtle(store,
                "@prefix ex: <http://example.org/> .\n"
                "@prefix rdfs: <http://www.w3.org/2000/01/rdf-schema#> .\n"
                "ex:A rdfs:subClassOf ex:B . ex:B rdfs:subClassOf ex:A .\n"
                "ex:x a ex:A .\n");
    store.build();
    RdfsStats stats = materialise_rdfs(store);
    CHECK(stats.rounds <= 64);
    CHECK_EQ(ask(store, "SELECT ?t WHERE { ex:x a ?t }").results.size(), std::size_t{2});
}
