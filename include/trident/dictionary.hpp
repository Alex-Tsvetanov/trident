// Two way mapping between RDF terms and 64 bit identifiers.
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "trident/term.hpp"

namespace trident {

class Dictionary {
public:
    // Interns the term and returns its identifier, creating one if needed.
    TermId intern(const Term& term);
    // Looks the term up without creating an entry. Returns kUnbound if absent,
    // which is what makes a query with an unknown constant answer empty fast.
    TermId lookup(const Term& term) const;

    // Reverse direction. Throws std::out_of_range on an identifier this
    // dictionary never handed out.
    const Term& decode(TermId id) const;
    bool contains(TermId id) const;

    std::size_t size() const { return terms_.size(); }

    // Bytes held by the dictionary: the term records, their string payloads and
    // the forward map. Used by the benchmark, which reports what it can count
    // rather than what the process happens to have resident.
    std::size_t memory_bytes() const;

    // Iteration over every interned term, in ordinal order.
    const std::vector<Term>& terms() const { return terms_; }

private:
    std::vector<Term> terms_;                            // ordinal - 1 -> term
    std::unordered_map<std::string, TermId> by_key_;     // N-Triples form -> id
};

}  // namespace trident
