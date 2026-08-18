#include "trident/query.hpp"

#include <chrono>
#include <sstream>

namespace trident {

namespace {

using Clock = std::chrono::steady_clock;

double ms_since(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

}  // namespace

std::string ResultSet::to_table(std::size_t max_rows) const {
    std::vector<std::size_t> widths;
    widths.reserve(columns.size());
    for (const std::string& column : columns) widths.push_back(column.size() + 1);

    std::size_t shown = std::min(max_rows, rows.size());
    std::vector<std::vector<std::string>> cells;
    for (std::size_t r = 0; r < shown; ++r) {
        std::vector<std::string> line;
        for (std::size_t c = 0; c < columns.size(); ++c) {
            std::string text = c < rows[r].size() && rows[r][c].kind != TermKind::Invalid
                                   ? rows[r][c].to_display()
                                   : "-";
            widths[c] = std::max(widths[c], text.size());
            line.push_back(std::move(text));
        }
        cells.push_back(std::move(line));
    }

    std::ostringstream out;
    for (std::size_t c = 0; c < columns.size(); ++c) {
        out << (c ? "  " : "") << "?" << columns[c]
            << std::string(widths[c] - columns[c].size() - 1, ' ');
    }
    out << "\n";
    for (std::size_t c = 0; c < columns.size(); ++c) {
        out << (c ? "  " : "") << std::string(widths[c], '-');
    }
    out << "\n";
    for (const std::vector<std::string>& line : cells) {
        for (std::size_t c = 0; c < line.size(); ++c) {
            out << (c ? "  " : "") << line[c] << std::string(widths[c] - line[c].size(), ' ');
        }
        out << "\n";
    }
    if (rows.size() > shown) {
        out << "... " << (rows.size() - shown) << " more solutions\n";
    }
    return out.str();
}

QueryOutcome run_query(TripleStore& store, std::string_view sparql,
                       const PlanOptions& options) {
    QueryOutcome outcome;

    auto t0 = Clock::now();
    Query query = parse_sparql(sparql);
    outcome.parse_ms = ms_since(t0);
    outcome.algebra_text = algebra_to_string(*query.root);

    auto t1 = Clock::now();
    Plan plan = build_plan(store, query, options);
    outcome.plan_ms = ms_since(t1);
    outcome.plan_text = plan.text;

    outcome.results.columns = plan.columns;

    auto t2 = Clock::now();
    Row seed(plan.variables.size(), kUnbound);
    plan.root->open(seed);
    Row row;
    std::vector<Row> raw;
    while (plan.root->next(row)) raw.push_back(row);
    outcome.execute_ms = ms_since(t2);

    const Dictionary& dictionary = store.dictionary();
    outcome.results.rows.reserve(raw.size());
    for (const Row& solution : raw) {
        std::vector<Term> decoded;
        decoded.reserve(plan.column_slots.size());
        for (int slot : plan.column_slots) {
            TermId id = slot >= 0 ? solution[static_cast<std::size_t>(slot)] : kUnbound;
            decoded.push_back(id.valid() ? dictionary.decode(id) : Term{});
        }
        outcome.results.rows.push_back(std::move(decoded));
    }
    return outcome;
}

std::size_t count_query(TripleStore& store, std::string_view sparql,
                        const PlanOptions& options) {
    Query query = parse_sparql(sparql);
    Plan plan = build_plan(store, query, options);
    Row seed(plan.variables.size(), kUnbound);
    plan.root->open(seed);
    Row row;
    std::size_t solutions = 0;
    while (plan.root->next(row)) ++solutions;
    return solutions;
}

}  // namespace trident
