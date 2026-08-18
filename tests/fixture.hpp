// Shared setup for the tests that need a loaded store.
#pragma once

#include <algorithm>
#include <string>
#include <vector>

#include "trident/query.hpp"
#include "trident/turtle.hpp"

namespace fixture {

inline const char* prefixes() {
    return "PREFIX ex: <http://example.org/>\n"
           "PREFIX rdfs: <http://www.w3.org/2000/01/rdf-schema#>\n";
}

inline trident::TripleStore load_small() {
    trident::TripleStore store;
    trident::load_turtle_file(store, std::string(TRIDENT_DATA_DIR) + "/small.ttl");
    store.build();
    return store;
}

inline trident::QueryOutcome ask(trident::TripleStore& store, const std::string& body,
                                 const trident::PlanOptions& options = {}) {
    return trident::run_query(store, prefixes() + body, options);
}

// The values of one column, as displayed, in result order.
inline std::vector<std::string> column(const trident::QueryOutcome& outcome,
                                       const std::string& name) {
    std::vector<std::string> out;
    auto it = std::find(outcome.results.columns.begin(), outcome.results.columns.end(), name);
    if (it == outcome.results.columns.end()) return out;
    std::size_t index = static_cast<std::size_t>(it - outcome.results.columns.begin());
    for (const std::vector<trident::Term>& row : outcome.results.rows) {
        out.push_back(index < row.size() && row[index].kind != trident::TermKind::Invalid
                          ? row[index].value
                          : std::string("<unbound>"));
    }
    return out;
}

inline std::vector<std::string> sorted_column(const trident::QueryOutcome& outcome,
                                              const std::string& name) {
    std::vector<std::string> out = column(outcome, name);
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace fixture
