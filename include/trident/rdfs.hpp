// RDFS entailment: materialisation into the store, or expansion at query time.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <unordered_map>
#include <vector>

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

// Read-only view of the RDFS vocabulary inside a store, closed under
// transitivity. Built once per query when PlanOptions::rdfs_query_time is set.
class RdfsSchema {
public:
    explicit RdfsSchema(TripleStore& store);

    TermId type_id() const { return type_; }
    // Properties that entail `property` through subPropertyOf* (including itself).
    std::vector<TermId> properties_entailing(TermId property) const;
    // Classes that entail `klass` through subClassOf* (including itself).
    std::vector<TermId> classes_entailing(TermId klass) const;
    // Properties whose domain includes klass or a subclass of klass.
    std::vector<TermId> properties_with_domain(TermId klass) const;
    // Properties whose range includes klass or a subclass of klass.
    std::vector<TermId> properties_with_range(TermId klass) const;

private:
    using Relation = std::unordered_map<std::uint64_t, std::vector<TermId>>;

    std::vector<TermId> lookup(const Relation& relation, TermId key) const;

    TermId type_ = kUnbound;
    Relation super_property_;
    Relation super_class_;
    Relation sub_property_;
    Relation sub_class_;
    Relation domain_of_;
    Relation range_of_;
};

}  // namespace trident
