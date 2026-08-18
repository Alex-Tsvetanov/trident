// The facade the demo, the tests and the benchmark all go through: parse, plan,
// execute, and report what each step cost.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "trident/planner.hpp"
#include "trident/sparql_parser.hpp"
#include "trident/store.hpp"

namespace trident {

struct ResultSet {
    std::vector<std::string> columns;
    // A term of kind Invalid stands for an unbound variable, which OPTIONAL and
    // a projection over a UNION both produce.
    std::vector<std::vector<Term>> rows;

    std::size_t size() const { return rows.size(); }
    // Fixed width rendering, at most max_rows lines of body.
    std::string to_table(std::size_t max_rows = 20) const;
};

struct QueryOutcome {
    ResultSet results;
    std::string algebra_text;
    std::string plan_text;
    double parse_ms = 0;
    double plan_ms = 0;
    double execute_ms = 0;
};

// Parses, plans and executes, decoding every solution into terms.
QueryOutcome run_query(TripleStore& store, std::string_view sparql,
                       const PlanOptions& options = {});

// Same pipeline without decoding: the engine runs to exhaustion and only the
// number of solutions comes back. This is what the benchmark times, so that the
// measurement is of the engine and not of string formatting.
std::size_t count_query(TripleStore& store, std::string_view sparql,
                        const PlanOptions& options = {});

}  // namespace trident
