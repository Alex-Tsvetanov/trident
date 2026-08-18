// SPARQL 1.1 SELECT queries into an algebra tree.
#pragma once

#include <string_view>

#include "trident/sparql_ast.hpp"

namespace trident {

// Throws ParseError with a line and a column. Constructs outside the supported
// subset are rejected with an explicit message rather than ignored, so a query
// never silently means something other than what it says.
Query parse_sparql(std::string_view text);

}  // namespace trident
