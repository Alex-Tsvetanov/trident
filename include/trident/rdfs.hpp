// RDFS entailment as a forward chaining pass over the store.
#pragma once

#include <cstddef>
#include <string_view>

#include "trident/store.hpp"

namespace trident {

// The four rules the pass implements, plus the two transitivity rules they need.
// This is the subset of RDF Schema that carries the weight in practice; the rules
// about container membership and the axiomatic triples are left out on purpose.
namespace rdfs_iri {
inline constexpr std::string_view kSubClassOf = "http://www.w3.org/2000/01/rdf-schema#subClassOf";
inline constexpr std::string_view kSubPropertyOf =
    "http://www.w3.org/2000/01/rdf-schema#subPropertyOf";
inline constexpr std::string_view kDomain = "http://www.w3.org/2000/01/rdf-schema#domain";
inline constexpr std::string_view kRange = "http://www.w3.org/2000/01/rdf-schema#range";
inline constexpr std::string_view kType = "http://www.w3.org/1999/02/22-rdf-syntax-ns#type";
}  // namespace rdfs_iri

struct RdfsStats {
    std::size_t before = 0;
    std::size_t after = 0;
    std::size_t inferred = 0;
    int rounds = 0;
};

// Applies the rules until nothing new appears, adds the inferred triples to the
// store and rebuilds the indexes. Idempotent: running it twice infers nothing
// the second time.
RdfsStats materialise_rdfs(TripleStore& store);

}  // namespace trident
