// The demo. One command, no arguments, no downloads: it generates the dataset,
// loads it, and runs a series of queries of increasing complexity, printing the
// algebra, the chosen plan, the number of solutions and the elapsed time for
// each one.
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#include "trident/generator.hpp"
#include "trident/query.hpp"
#include "trident/rdfs.hpp"
#include "trident/turtle.hpp"

using namespace trident;
using Clock = std::chrono::steady_clock;

namespace {

double ms_since(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

void rule(char c = '=') { std::cout << std::string(78, c) << "\n"; }

void heading(const std::string& text) {
    std::cout << "\n";
    rule();
    std::cout << text << "\n";
    rule();
}

struct DemoQuery {
    const char* title;
    const char* text;
};

const DemoQuery kQueries[] = {
    {"Q1  one pattern, LIMIT stops the scan early",
     "PREFIX ex: <http://example.org/>\n"
     "SELECT ?paper ?title WHERE { ?paper ex:title ?title } LIMIT 5"},

    {"Q2  two patterns, the planner starts from the more selective one",
     "PREFIX ex: <http://example.org/>\n"
     "SELECT ?title WHERE {\n"
     "  ?paper ex:venue ex:venue3 .\n"
     "  ?paper ex:title ?title .\n"
     "} LIMIT 5"},

    {"Q3  three patterns and a FILTER over a numeric literal",
     "PREFIX ex: <http://example.org/>\n"
     "SELECT ?title ?year ?name WHERE {\n"
     "  ?paper ex:year ?year .\n"
     "  ?paper ex:title ?title .\n"
     "  ?paper ex:mainAuthor ?a .\n"
     "  ?a ex:name ?name .\n"
     "  FILTER (?year >= 2024 && CONTAINS(?title, \"indexing\"))\n"
     "} LIMIT 5"},

    {"Q4  OPTIONAL: authors keep their row even without a homepage",
     "PREFIX ex: <http://example.org/>\n"
     "SELECT ?name ?homepage WHERE {\n"
     "  ?a a ex:Author .\n"
     "  ?a ex:name ?name .\n"
     "  OPTIONAL { ?a ex:homepage ?homepage }\n"
     "} LIMIT 8"},

    {"Q5  UNION of two alternatives",
     "PREFIX ex: <http://example.org/>\n"
     "SELECT DISTINCT ?thing WHERE {\n"
     "  { ?thing a ex:Venue } UNION { ?thing a ex:Topic }\n"
     "} ORDER BY ?thing LIMIT 8"},

    {"Q6  DISTINCT, ORDER BY and OFFSET",
     "PREFIX ex: <http://example.org/>\n"
     "SELECT DISTINCT ?year WHERE { ?paper ex:year ?year }\n"
     "ORDER BY DESC(?year) LIMIT 6 OFFSET 2"},

    {"Q7  GROUP BY with COUNT, sorted by the aggregate",
     "PREFIX ex: <http://example.org/>\n"
     "SELECT ?venue (COUNT(?paper) AS ?papers) WHERE {\n"
     "  ?paper ex:venue ?venue .\n"
     "} GROUP BY ?venue ORDER BY DESC(?papers) LIMIT 6"},

    {"Q8  several aggregates over one group",
     "PREFIX ex: <http://example.org/>\n"
     "SELECT ?topic (COUNT(*) AS ?n) (AVG(?c) AS ?avgCitations) (MAX(?c) AS ?best) WHERE {\n"
     "  ?paper ex:topic ?topic .\n"
     "  ?paper ex:citationCount ?c .\n"
     "} GROUP BY ?topic ORDER BY DESC(?avgCitations) LIMIT 6"},

    {"Q9  a join on the object position, where both scans arrive sorted",
     "PREFIX ex: <http://example.org/>\n"
     "SELECT ?a ?first ?second WHERE {\n"
     "  ?first ex:mainAuthor ?a .\n"
     "  ?second ex:author ?a .\n"
     "} LIMIT 5"},

    {"Q10 a four pattern chain, the shape where join order matters most",
     "PREFIX ex: <http://example.org/>\n"
     "SELECT ?title ?org WHERE {\n"
     "  ?paper ex:mainAuthor ?a .\n"
     "  ?a ex:affiliation ?org .\n"
     "  ?paper ex:title ?title .\n"
     "  ?paper ex:venue ex:venue1 .\n"
     "} LIMIT 5"},
};

}  // namespace

int main() {
    std::cout << "Trident: an embedded RDF triple store with a SPARQL 1.1 query engine\n";

    heading("1. The dataset is generated here, not downloaded");
    DatasetSpec spec;
    spec.papers = 1500;
    spec.authors = 300;
    auto t_gen = Clock::now();
    std::string turtle = generate_dataset(spec);
    double generate_ms = ms_since(t_gen);
    const std::string path = "trident_demo_dataset.ttl";
    {
        std::ofstream file(path, std::ios::binary);
        file << turtle;
    }
    std::cout << "generated " << turtle.size() << " bytes of Turtle in " << std::fixed
              << std::setprecision(1) << generate_ms << " ms, written to " << path << "\n";

    heading("2. Loading, dictionary encoding and index build");
    TripleStore store;
    auto t_parse = Clock::now();
    std::size_t parsed = load_turtle_file(store, path);
    double parse_ms = ms_since(t_parse);
    auto t_build = Clock::now();
    store.build();
    double build_ms = ms_since(t_build);

    std::cout << "parsed          " << parsed << " triples in " << parse_ms << " ms ("
              << std::setprecision(0) << (parsed / (parse_ms / 1000.0)) << " triples/s)\n"
              << std::setprecision(1) << "indexed         " << store.triple_count()
              << " distinct triples in " << build_ms << " ms\n"
              << "dictionary      " << store.dictionary().size() << " terms\n"
              << "structures      " << (store.memory_bytes() / 1024) << " KiB in memory\n";

    heading("3. Queries");
    for (const DemoQuery& demo : kQueries) {
        rule('-');
        std::cout << demo.title << "\n\n" << demo.text << "\n\n";
        QueryOutcome outcome = run_query(store, demo.text);
        std::cout << "algebra:\n" << outcome.algebra_text << "\nplan:\n"
                  << outcome.plan_text << "\n"
                  << outcome.results.size() << " solutions; parse " << std::setprecision(3)
                  << outcome.parse_ms << " ms, plan " << outcome.plan_ms << " ms, execute "
                  << outcome.execute_ms << " ms\n\n"
                  << outcome.results.to_table(8) << "\n";
    }

    heading("4. What the planner is worth");
    const char* heavy =
        "PREFIX ex: <http://example.org/>\n"
        "SELECT ?title ?name WHERE {\n"
        "  ?paper ex:title ?title .\n"
        "  ?paper ex:mainAuthor ?a .\n"
        "  ?a ex:name ?name .\n"
        "  ?a ex:affiliation ex:org3 .\n"
        "  ?paper ex:venue ex:venue2 .\n"
        "}";
    std::cout << heavy << "\n\n";
    for (bool naive : {false, true}) {
        PlanOptions options;
        options.naive_join_order = naive;
        QueryOutcome outcome = run_query(store, heavy, options);
        std::cout << (naive ? "left to right order" : "planned order      ") << ": "
                  << outcome.results.size() << " solutions in " << std::setprecision(3)
                  << outcome.execute_ms << " ms\n";
        if (!naive) std::cout << "\nchosen plan:\n" << outcome.plan_text << "\n";
    }

    heading("5. RDFS entailment as a materialisation pass");
    const char* inferred_query =
        "PREFIX ex: <http://example.org/>\n"
        "SELECT (COUNT(?x) AS ?agents) WHERE { ?x a ex:Agent }";
    std::cout << inferred_query << "\n\n";
    std::cout << "before inference: " << run_query(store, inferred_query).results.to_table(1);
    auto t_rdfs = Clock::now();
    RdfsStats stats = materialise_rdfs(store);
    double rdfs_ms = ms_since(t_rdfs);
    std::cout << "\nforward chaining added " << stats.inferred << " triples in " << stats.rounds
              << " rounds, " << std::setprecision(1) << rdfs_ms << " ms; the store now holds "
              << stats.after << " triples\n\n"
              << "after inference:  " << run_query(store, inferred_query).results.to_table(1);

    std::cout << "\ndone.\n";
    return 0;
}
