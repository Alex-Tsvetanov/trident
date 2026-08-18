// Turtle and N-Triples reading. N-Triples is a subset of Turtle, so one parser
// covers both; the strict flag turns off everything Turtle adds.
#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>

#include "trident/lexer.hpp"
#include "trident/store.hpp"
#include "trident/term.hpp"

namespace trident {

struct TurtleOptions {
    // Base IRI for relative references. May be replaced by a @base directive.
    std::string base;
    // Blank node labels are local to the document they come from. Every label is
    // rewritten as scope + "_" + label so that two documents loaded into the same
    // store cannot collide. An empty scope keeps the labels as written, which is
    // what the tests want.
    std::string blank_scope;
    // Rejects every construct that Turtle adds on top of N-Triples.
    bool ntriples_only = false;
};

using TripleSink = std::function<void(const Term&, const Term&, const Term&)>;

// Parses the text and hands every triple to the sink. Throws ParseError with a
// line and a column on the first syntax error, having emitted nothing further.
std::size_t parse_turtle(std::string_view text, const TripleSink& sink,
                         const TurtleOptions& options = {});

// Convenience wrappers. Both stage triples in the store; the caller still has to
// call TripleStore::build().
std::size_t load_turtle(TripleStore& store, std::string_view text,
                        const TurtleOptions& options = {});
std::size_t load_turtle_file(TripleStore& store, const std::string& path,
                             const TurtleOptions& options = {});

// Reads a whole file into memory. Throws std::runtime_error when it cannot.
std::string read_file(const std::string& path);

}  // namespace trident
