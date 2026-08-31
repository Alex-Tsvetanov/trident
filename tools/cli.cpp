// A small command line front end, so that the library can be tried on a file
// without writing a program.
#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

#include "trident/generator.hpp"
#include "trident/query.hpp"
#include "trident/rdfs.hpp"
#include "trident/turtle.hpp"

using namespace trident;

namespace {

void usage() {
    std::cout <<
        "usage: trident_cli [options] [file.ttl|file.nq ...]\n"
        "\n"
        "  -q, --query TEXT   the SPARQL query; without it the query is read from\n"
        "                     standard input\n"
        "      --ntriples     parse the input files as N-Triples, rejecting every\n"
        "                     construct that Turtle adds\n"
        "      --nquads       parse the input files as N-Quads\n"
        "      --rdfs         run the RDFS materialisation pass after loading\n"
        "      --rdfs-query   apply RDFS rules at query time, without materialising\n"
        "      --naive        keep the join order as written, without planning\n"
        "      --no-merge     never use merge join\n"
        "      --generate N   load a generated dataset of N papers instead of a file\n"
        "      --save PATH    write the built store to PATH (.trident)\n"
        "      --plain        with --save, write uncompressed mmap-ready indexes\n"
        "      --open PATH    reopen a store saved with --save, skipping the rebuild\n"
        "      --plan         print the algebra and the chosen plan\n"
        "      --limit N      print at most N solutions (default 20)\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> files;
    std::string query_text, save_path, open_path;
    bool ntriples = false, nquads = false, rdfs = false, show_plan = false, plain = false;
    std::size_t generate = 0, print_limit = 20;
    PlanOptions options;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "-q" || arg == "--query") && i + 1 < argc) query_text = argv[++i];
        else if (arg == "--ntriples") ntriples = true;
        else if (arg == "--nquads") nquads = true;
        else if (arg == "--rdfs") rdfs = true;
        else if (arg == "--rdfs-query") options.rdfs_query_time = true;
        else if (arg == "--naive") options.naive_join_order = true;
        else if (arg == "--no-merge") options.enable_merge_join = false;
        else if (arg == "--plan") show_plan = true;
        else if (arg == "--plain") plain = true;
        else if (arg == "--generate" && i + 1 < argc) generate = std::stoul(argv[++i]);
        else if (arg == "--save" && i + 1 < argc) save_path = argv[++i];
        else if (arg == "--open" && i + 1 < argc) open_path = argv[++i];
        else if (arg == "--limit" && i + 1 < argc) print_limit = std::stoul(argv[++i]);
        else if (arg == "-h" || arg == "--help") { usage(); return 0; }
        else if (!arg.empty() && arg[0] == '-') {
            std::cerr << "trident: unknown option " << arg << "\n";
            return 2;
        } else {
            files.push_back(arg);
        }
    }

    if (files.empty() && generate == 0 && open_path.empty()) {
        std::cerr << "trident: nothing to load; give a file, --open, or --generate\n";
        usage();
        return 2;
    }

    TripleStore store;
    try {
        if (!open_path.empty()) {
            store = TripleStore::open(open_path);
        } else {
            if (generate > 0) {
                DatasetSpec spec;
                spec.papers = generate;
                spec.authors = std::max<std::size_t>(20, generate / 5);
                load_turtle(store, generate_dataset(spec));
            }
            TurtleOptions parse_options;
            parse_options.ntriples_only = ntriples;
            for (const std::string& file : files) {
                if (nquads) load_nquads_file(store, file, parse_options);
                else load_turtle_file(store, file, parse_options);
            }
            store.build();
        }
    } catch (const ParseError& error) {
        std::cerr << "trident: " << error.what() << "\n";
        return 1;
    } catch (const std::exception& error) {
        std::cerr << "trident: " << error.what() << "\n";
        return 1;
    }

    std::cerr << "loaded " << store.triple_count() << " triples, "
              << store.quad_count() << " quads, "
              << store.dictionary().size() << " terms, "
              << (store.memory_bytes() / 1024) << " KiB\n";

    if (rdfs) {
        RdfsStats stats = materialise_rdfs(store);
        std::cerr << "inference added " << stats.inferred << " triples in " << stats.rounds
                  << " rounds\n";
    }

    if (!save_path.empty()) {
        SaveOptions save;
        save.codec = plain ? StoreCodec::Plain : StoreCodec::Compressed;
        store.save(save_path, save);
        std::cerr << "wrote " << save_path << "\n";
    }

    if (query_text.empty()) {
        std::string line;
        while (std::getline(std::cin, line)) query_text += line + "\n";
    }
    if (query_text.find_first_not_of(" \t\r\n") == std::string::npos) {
        if (!save_path.empty()) return 0;
        std::cerr << "trident: no query given\n";
        return 2;
    }

    try {
        QueryOutcome outcome = run_query(store, query_text, options);
        if (show_plan) {
            std::cout << "algebra:\n" << outcome.algebra_text << "\nplan:\n"
                      << outcome.plan_text << "\n";
        }
        std::cout << outcome.results.to_table(print_limit);
        std::cerr << outcome.results.size() << " solutions; parse " << outcome.parse_ms
                  << " ms, plan " << outcome.plan_ms << " ms, execute " << outcome.execute_ms
                  << " ms\n";
    } catch (const ParseError& error) {
        std::cerr << "trident: " << error.what() << "\n";
        return 1;
    } catch (const std::exception& error) {
        std::cerr << "trident: " << error.what() << "\n";
        return 1;
    }
    return 0;
}
