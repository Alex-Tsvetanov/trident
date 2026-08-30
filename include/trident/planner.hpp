// The query planner: it turns an algebra tree into an operator tree, fixes the
// join order from index statistics, and picks an index and a join strategy for
// every pattern.
#pragma once

#include <string>
#include <vector>

#include "trident/exec.hpp"
#include "trident/sparql_ast.hpp"
#include "trident/store.hpp"

namespace trident {

struct PlanOptions {
    // Keeps the patterns of every basic graph pattern in the order they were
    // written. This exists so that the cost of planning can be measured against
    // the cost of not planning, rather than assumed.
    bool naive_join_order = false;
    // Merge join needs both inputs sorted on the join variable, which only holds
    // where both are index scans. Turning it off falls back to index nested loop
    // join everywhere, which is the other half of the same measurement.
    bool enable_merge_join = true;
    // Applies RDFS rules while scanning, without inserting entailed triples into
    // the store. Orthogonal to materialise_rdfs(): either mode, both, or neither.
    bool rdfs_query_time = false;
};

struct Plan {
    OperatorPtr root;
    std::vector<std::string> variables;  // slot number -> variable name
    std::vector<std::string> columns;    // projected names, in order
    std::vector<int> column_slots;
    std::string text;                    // the printable plan
    std::size_t estimated_rows = 0;      // product of the pattern cardinalities
};

// The query is taken by reference and annotated: variable slots are written into
// the expression nodes, and the plan holds pointers into the query's expressions.
// The query must therefore outlive the plan.
Plan build_plan(TripleStore& store, Query& query, const PlanOptions& options = {});

}  // namespace trident
