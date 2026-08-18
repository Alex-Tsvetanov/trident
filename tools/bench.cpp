// The benchmark. Every number the report quotes comes from here.
//
// Method: each configuration is run repeatedly, the first runs are discarded so
// that the measurement does not include warming the allocator and the file
// cache, and the median of the rest is reported. Nothing is estimated, and the
// solution count is printed alongside every timing, because a configuration that
// returns fewer solutions is faster for a reason that is not speed.
#include <algorithm>
#include <chrono>
#include <cstdlib>
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

int g_repeats = 7;
int g_warmups = 2;

double ms_since(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

double median(std::vector<double> values) {
    if (values.empty()) return 0;
    std::sort(values.begin(), values.end());
    std::size_t middle = values.size() / 2;
    if (values.size() % 2 == 1) return values[middle];
    return (values[middle - 1] + values[middle]) / 2.0;
}

template <typename F>
double timed_median(F&& body) {
    std::vector<double> samples;
    for (int i = 0; i < g_warmups + g_repeats; ++i) {
        auto start = Clock::now();
        body();
        double elapsed = ms_since(start);
        if (i >= g_warmups) samples.push_back(elapsed);
    }
    return median(std::move(samples));
}

void rule() { std::cout << std::string(78, '-') << "\n"; }

struct Corpus {
    std::size_t papers;
    std::string turtle;
    std::size_t triples = 0;
};

Corpus make_corpus(std::size_t papers) {
    DatasetSpec spec;
    spec.papers = papers;
    spec.authors = std::max<std::size_t>(20, papers / 5);
    spec.organisations = std::max<std::size_t>(5, papers / 50);
    spec.venues = std::max<std::size_t>(4, papers / 100);
    Corpus corpus;
    corpus.papers = papers;
    corpus.turtle = generate_dataset(spec);
    return corpus;
}

struct QueryCase {
    const char* name;
    const char* text;
};

const QueryCase kWorkload[] = {
    {"B1 single pattern, bound predicate",
     "PREFIX ex: <http://example.org/>\n"
     "SELECT ?p ?t WHERE { ?p ex:title ?t }"},
    {"B2 star join, two patterns",
     "PREFIX ex: <http://example.org/>\n"
     "SELECT ?t ?y WHERE { ?p ex:title ?t . ?p ex:year ?y }"},
    {"B3 four pattern chain, one selective constant",
     "PREFIX ex: <http://example.org/>\n"
     "SELECT ?t ?n WHERE { ?p ex:title ?t . ?p ex:mainAuthor ?a . ?a ex:name ?n . "
     "?a ex:affiliation ex:org3 }"},
    {"B4 five pattern chain, two selective constants",
     "PREFIX ex: <http://example.org/>\n"
     "SELECT ?t ?n WHERE { ?p ex:title ?t . ?p ex:mainAuthor ?a . ?a ex:name ?n . "
     "?a ex:affiliation ex:org3 . ?p ex:venue ex:venue2 }"},
    {"B5 filter over a numeric literal",
     "PREFIX ex: <http://example.org/>\n"
     "SELECT ?p WHERE { ?p ex:citationCount ?c FILTER (?c > 100) }"},
    {"B6 optional",
     "PREFIX ex: <http://example.org/>\n"
     "SELECT ?n ?h WHERE { ?a a ex:Author . ?a ex:name ?n OPTIONAL { ?a ex:homepage ?h } }"},
    {"B7 group by with count",
     "PREFIX ex: <http://example.org/>\n"
     "SELECT ?v (COUNT(?p) AS ?n) WHERE { ?p ex:venue ?v } GROUP BY ?v"},
    {"B8 order by over the whole relation",
     "PREFIX ex: <http://example.org/>\n"
     "SELECT ?p WHERE { ?p ex:citationCount ?c } ORDER BY DESC(?c)"},
};

// The shape where both inputs come out of the index already sorted on the join
// variable, which is the only shape where merge join is legal.
const char* kMergeJoinQuery =
    "PREFIX ex: <http://example.org/>\n"
    "SELECT ?a ?first ?second WHERE { ?first ex:mainAuthor ?a . ?second ex:author ?a }";

void report_loading(const std::vector<std::size_t>& sizes) {
    std::cout << "\nLoading and index build\n";
    rule();
    std::cout << std::left << std::setw(10) << "papers" << std::setw(11) << "triples"
              << std::setw(11) << "bytes" << std::setw(11) << "parse ms" << std::setw(12)
              << "triples/s" << std::setw(11) << "build ms" << std::setw(10) << "KiB"
              << "terms\n";
    rule();
    for (std::size_t papers : sizes) {
        Corpus corpus = make_corpus(papers);

        std::size_t parsed = 0;
        double parse_ms = timed_median([&] {
            TripleStore store;
            parsed = load_turtle(store, corpus.turtle);
        });

        // Loading and index building are timed apart, because they are different
        // costs: one is parsing and interning, the other is three sorts.
        TripleStore store;
        load_turtle(store, corpus.turtle);
        double build_ms = timed_median([&] { store.build(); });

        std::cout << std::left << std::setw(10) << papers << std::setw(11)
                  << store.triple_count() << std::setw(11) << corpus.turtle.size()
                  << std::setw(11) << std::fixed << std::setprecision(1) << parse_ms
                  << std::setw(12) << std::setprecision(0)
                  << (static_cast<double>(parsed) / (parse_ms / 1000.0)) << std::setw(11)
                  << std::setprecision(1) << build_ms << std::setw(10)
                  << (store.memory_bytes() / 1024) << store.dictionary().size() << "\n";
    }
}

void report_queries(std::size_t papers) {
    Corpus corpus = make_corpus(papers);
    TripleStore store;
    load_turtle(store, corpus.turtle);
    store.build();

    std::cout << "\nQuery latency, planner on against off (" << store.triple_count()
              << " triples)\n";
    rule();
    std::cout << std::left << std::setw(48) << "query" << std::setw(12) << "planned ms"
              << std::setw(12) << "naive ms" << std::setw(9) << "ratio" << "solutions\n";
    rule();
    for (const QueryCase& item : kWorkload) {
        PlanOptions planned;
        PlanOptions naive;
        naive.naive_join_order = true;

        std::size_t solutions = count_query(store, item.text, planned);
        std::size_t naive_solutions = count_query(store, item.text, naive);

        double planned_ms = timed_median([&] { count_query(store, item.text, planned); });
        double naive_ms = timed_median([&] { count_query(store, item.text, naive); });

        std::cout << std::left << std::setw(48) << item.name << std::setw(12) << std::fixed
                  << std::setprecision(3) << planned_ms << std::setw(12) << naive_ms
                  << std::setw(9) << std::setprecision(2)
                  << (planned_ms > 0 ? naive_ms / planned_ms : 0.0) << solutions;
        if (solutions != naive_solutions) {
            std::cout << "  MISMATCH " << naive_solutions;
        }
        std::cout << "\n";
    }

    std::cout << "\nJoin strategy on the shape where both inputs arrive sorted\n";
    rule();
    PlanOptions with_merge;
    PlanOptions without_merge;
    without_merge.enable_merge_join = false;
    std::size_t merged_rows = count_query(store, kMergeJoinQuery, with_merge);
    std::size_t looped_rows = count_query(store, kMergeJoinQuery, without_merge);
    double merged_ms = timed_median([&] { count_query(store, kMergeJoinQuery, with_merge); });
    double looped_ms = timed_median([&] { count_query(store, kMergeJoinQuery, without_merge); });
    std::cout << std::left << std::setw(30) << "merge join" << std::fixed << std::setprecision(3)
              << std::setw(12) << merged_ms << merged_rows << " solutions\n"
              << std::setw(30) << "index nested loop join" << std::setw(12) << looped_ms
              << looped_rows << " solutions\n";
    if (merged_rows != looped_rows) std::cout << "MISMATCH: the two strategies disagree\n";

    std::cout << "\nSelectivity of the leading pattern against total work\n";
    rule();
    std::cout << std::left << std::setw(30) << "leading constant" << std::setw(14)
              << "cardinality" << std::setw(12) << "planned ms" << "solutions\n";
    for (const char* org : {"ex:org1", "ex:org3", "ex:org5"}) {
        std::string text = std::string("PREFIX ex: <http://example.org/>\n"
                                       "SELECT ?t ?n WHERE { ?p ex:title ?t . ?p ex:mainAuthor ?a . "
                                       "?a ex:name ?n . ?a ex:affiliation ") +
                           org + " }";
        EncodedPattern pattern;
        pattern.p = store.encode(Term::iri("http://example.org/affiliation"));
        pattern.o = store.encode(Term::iri(std::string("http://example.org/") + (org + 3)));
        std::size_t cardinality = store.count(pattern);
        std::size_t solutions = count_query(store, text);
        double elapsed = timed_median([&] { count_query(store, text); });
        std::cout << std::left << std::setw(30) << org << std::setw(14) << cardinality
                  << std::setw(12) << std::fixed << std::setprecision(3) << elapsed << solutions
                  << "\n";
    }
}

void report_inference(std::size_t papers) {
    Corpus corpus = make_corpus(papers);
    std::cout << "\nRDFS materialisation\n";
    rule();
    RdfsStats stats;
    double elapsed = timed_median([&] {
        TripleStore store;
        load_turtle(store, corpus.turtle);
        store.build();
        stats = materialise_rdfs(store);
    });
    // The timing above includes loading, so measure the pass on its own as well.
    TripleStore store;
    load_turtle(store, corpus.turtle);
    store.build();
    std::size_t before_bytes = store.memory_bytes();
    auto start = Clock::now();
    RdfsStats once = materialise_rdfs(store);
    double pass_ms = ms_since(start);

    std::cout << "triples before        " << once.before << "\n"
              << "triples inferred      " << once.inferred << "\n"
              << "triples after         " << once.after << "\n"
              << "rounds to fixpoint    " << once.rounds << "\n"
              << "pass, single run ms   " << std::fixed << std::setprecision(1) << pass_ms << "\n"
              << "load and pass, median " << elapsed << " ms\n"
              << "structures before KiB " << (before_bytes / 1024) << "\n"
              << "structures after KiB  " << (store.memory_bytes() / 1024) << "\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::size_t> sizes = {1000, 5000, 20000, 50000};
    std::size_t query_size = 20000;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--quick") {
            sizes = {1000, 5000};
            query_size = 5000;
            g_repeats = 3;
            g_warmups = 1;
        } else if (arg == "--repeats" && i + 1 < argc) {
            g_repeats = std::atoi(argv[++i]);
        } else if (arg == "--help") {
            std::cout << "usage: trident_bench [--quick] [--repeats N]\n";
            return 0;
        }
    }

    std::cout << "Trident benchmark\n"
              << "repeats " << g_repeats << ", warmup runs discarded " << g_warmups
              << ", median reported\n";
    report_loading(sizes);
    report_queries(query_size);
    report_inference(query_size);
    std::cout << "\ndone.\n";
    return 0;
}
