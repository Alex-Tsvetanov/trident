#include "harness.hpp"

#include <sstream>
#include <variant>

#include "fixture.hpp"
#include "trident/query.hpp"
#include "trident/sparql_parser.hpp"
#include "trident/store.hpp"
#include "trident/turtle.hpp"

using namespace trident;

TEST(graphs, nquads_load_into_default_and_named_graphs) {
    TripleStore store;
    std::size_t n = load_nquads(
        store,
        "<http://a/s> <http://a/p> <http://a/o> .\n"
        "<http://a/s2> <http://a/p> \"x\" <http://a/g1> .\n"
        "<http://a/s3> <http://a/p> <http://a/o3> <http://a/g2> .\n");
    store.build();
    CHECK_EQ(n, std::size_t{3});
    CHECK_EQ(store.triple_count(), std::size_t{1});
    CHECK_EQ(store.quad_count(), std::size_t{3});
    CHECK_EQ(store.named_graphs().size(), std::size_t{2});
}

TEST(graphs, graph_iri_pattern_sees_only_that_named_graph) {
    TripleStore store;
    store.add(Term::iri("http://a/s"), Term::iri("http://a/p"), Term::iri("http://a/default"));
    store.add(Term::iri("http://a/s"), Term::iri("http://a/p"), Term::iri("http://a/named"),
              Term::iri("http://a/g"));
    store.build();

    QueryOutcome outside = run_query(store, "SELECT ?o WHERE { <http://a/s> <http://a/p> ?o }");
    CHECK_EQ(outside.results.size(), std::size_t{1});
    CHECK_EQ(outside.results.rows[0][0].value, std::string("http://a/default"));

    QueryOutcome inside = run_query(
        store, "SELECT ?o WHERE { GRAPH <http://a/g> { <http://a/s> <http://a/p> ?o } }");
    CHECK_EQ(inside.results.size(), std::size_t{1});
    CHECK_EQ(inside.results.rows[0][0].value, std::string("http://a/named"));
}

TEST(graphs, graph_variable_binds_each_named_graph) {
    TripleStore store;
    store.add(Term::iri("http://a/s"), Term::iri("http://a/p"), Term::iri("http://a/o1"),
              Term::iri("http://a/g1"));
    store.add(Term::iri("http://a/s"), Term::iri("http://a/p"), Term::iri("http://a/o2"),
              Term::iri("http://a/g2"));
    store.build();

    QueryOutcome outcome =
        run_query(store, "SELECT ?g ?o WHERE { GRAPH ?g { <http://a/s> <http://a/p> ?o } } "
                         "ORDER BY ?g");
    CHECK_EQ(outcome.results.size(), std::size_t{2});
    CHECK_EQ(outcome.results.rows[0][0].value, std::string("http://a/g1"));
    CHECK_EQ(outcome.results.rows[1][0].value, std::string("http://a/g2"));
}

TEST(graphs, graph_clause_parses_into_algebra) {
    Query query = parse_sparql("SELECT ?s WHERE { GRAPH <http://a/g> { ?s ?p ?o } }");
    CHECK(std::holds_alternative<GraphNode>(
        std::get<ProjectNode>(query.root->node).child->node));
}

TEST(graphs, nquads_output_round_trips) {
    TripleStore first;
    first.add(Term::iri("http://a/s"), Term::iri("http://a/p"), Term::iri("http://a/o"));
    first.add(Term::iri("http://a/s2"), Term::iri("http://a/p"), Term::iri("http://a/o2"),
              Term::iri("http://a/g"));
    first.build();
    std::ostringstream written;
    first.write_nquads(written);

    TripleStore second;
    load_nquads(second, written.str());
    second.build();
    CHECK_EQ(second.triple_count(), first.triple_count());
    CHECK_EQ(second.quad_count(), first.quad_count());
}
