#include "trident/planner.hpp"

#include "trident/rdfs.hpp"

#include <algorithm>
#include <memory>
#include <sstream>

namespace trident {

namespace {

class PlanBuilder {
public:
    PlanBuilder(TripleStore& store, Query& query, const PlanOptions& options)
        : store_(store), query_(query), options_(options) {
        if (options_.rdfs_query_time) {
            schema_ = std::make_unique<RdfsSchema>(store_);
        }
    }

    Plan run() {
        Plan plan;
        collect_algebra_variables(*query_.root, plan.variables);
        for (const std::string& name : query_.projection) {
            if (std::find(plan.variables.begin(), plan.variables.end(), name) ==
                plan.variables.end()) {
                plan.variables.push_back(name);
            }
        }
        variables_ = plan.variables;
        resolve_slots(*query_.root);

        plan.root = compile(*query_.root);
        plan.columns = query_.projection;
        for (const std::string& name : query_.projection) {
            plan.column_slots.push_back(slot_of(name));
        }
        plan.estimated_rows = estimate_;
        plan.text = describe_plan(*plan.root);
        return plan;
    }

private:
    int slot_of(const std::string& name) const {
        auto it = std::find(variables_.begin(), variables_.end(), name);
        return it == variables_.end() ? -1 : static_cast<int>(it - variables_.begin());
    }

    void resolve_slots(Algebra& algebra) {
        std::visit(
            [&](auto& node) {
                using T = std::decay_t<decltype(node)>;
                if constexpr (std::is_same_v<T, BgpNode>) {
                } else if constexpr (std::is_same_v<T, JoinNode> ||
                                     std::is_same_v<T, UnionNode>) {
                    resolve_slots(*node.left);
                    resolve_slots(*node.right);
                } else if constexpr (std::is_same_v<T, LeftJoinNode>) {
                    resolve_slots(*node.left);
                    resolve_slots(*node.right);
                    if (node.condition) resolve_expression_slots(*node.condition, variables_);
                } else if constexpr (std::is_same_v<T, FilterNode>) {
                    resolve_slots(*node.child);
                    resolve_expression_slots(*node.condition, variables_);
                } else if constexpr (std::is_same_v<T, GraphNode>) {
                    resolve_slots(*node.child);
                } else if constexpr (std::is_same_v<T, GroupNode>) {
                    resolve_slots(*node.child);
                    for (Aggregate& agg : node.aggregates) {
                        if (agg.argument) resolve_expression_slots(*agg.argument, variables_);
                    }
                } else if constexpr (std::is_same_v<T, OrderByNode>) {
                    resolve_slots(*node.child);
                    for (OrderKey& key : node.keys) {
                        resolve_expression_slots(*key.expr, variables_);
                    }
                } else {
                    resolve_slots(*node.child);
                }
            },
            algebra.node);
    }

    CompiledPattern compile_pattern(const TriplePattern& pattern) {
        CompiledPattern out;
        out.text = pattern.to_string();
        const PatternTerm* positions[3] = {&pattern.s, &pattern.p, &pattern.o};
        for (int i = 0; i < 3; ++i) {
            if (positions[i]->is_variable) {
                out.slot[i] = slot_of(positions[i]->variable);
            } else {
                TermId id = store_.encode(positions[i]->constant);
                if (!id.valid()) out.impossible = true;
                out.constant[i] = id;
            }
        }
        return out;
    }

    static void pattern_variables(const CompiledPattern& pattern, std::vector<int>& out) {
        for (int i = 0; i < 3; ++i) {
            if (pattern.slot[i] >= 0 &&
                std::find(out.begin(), out.end(), pattern.slot[i]) == out.end()) {
                out.push_back(pattern.slot[i]);
            }
        }
    }

    static bool shares_variable(const CompiledPattern& pattern, const std::vector<int>& bound) {
        for (int i = 0; i < 3; ++i) {
            if (pattern.slot[i] >= 0 &&
                std::find(bound.begin(), bound.end(), pattern.slot[i]) != bound.end()) {
                return true;
            }
        }
        return false;
    }

    std::vector<std::size_t> order_patterns(const std::vector<CompiledPattern>& patterns,
                                            const std::vector<std::size_t>& cardinalities) {
        std::vector<std::size_t> order;
        if (options_.naive_join_order) {
            for (std::size_t i = 0; i < patterns.size(); ++i) order.push_back(i);
            return order;
        }
        std::vector<bool> used(patterns.size(), false);
        std::vector<int> bound;
        for (std::size_t step = 0; step < patterns.size(); ++step) {
            std::size_t best = patterns.size();
            bool best_connected = false;
            for (std::size_t i = 0; i < patterns.size(); ++i) {
                if (used[i]) continue;
                bool connected = !bound.empty() && shares_variable(patterns[i], bound);
                if (best == patterns.size()) {
                    best = i;
                    best_connected = connected;
                    continue;
                }
                if (connected != best_connected) {
                    if (connected) {
                        best = i;
                        best_connected = true;
                    }
                    continue;
                }
                if (cardinalities[i] < cardinalities[best]) best = i;
            }
            used[best] = true;
            order.push_back(best);
            pattern_variables(patterns[best], bound);
        }
        return order;
    }

    OperatorPtr make_scan(CompiledPattern pattern, const std::vector<int>& prebound) {
        return std::make_unique<IndexScan>(store_, std::move(pattern), prebound, graph_constant_,
                                           graph_slot_);
    }

    // Expands one compiled pattern under query-time RDFS into a (possibly
    // distinct) union of scans covering subproperties, subclasses, domain and
    // range.
    OperatorPtr make_rdfs_scan(CompiledPattern pattern, const std::vector<int>& prebound) {
        if (!schema_ || pattern.impossible) {
            return make_scan(std::move(pattern), prebound);
        }

        std::vector<OperatorPtr> arms;

        const bool type_query =
            pattern.constant[1].valid() && schema_->type_id().valid() &&
            pattern.constant[1] == schema_->type_id() && pattern.constant[2].valid();

        if (type_query) {
            for (TermId klass : schema_->classes_entailing(pattern.constant[2])) {
                CompiledPattern alt = pattern;
                alt.constant[2] = klass;
                arms.push_back(make_scan(std::move(alt), prebound));
            }
            auto domain_props = schema_->properties_with_domain(pattern.constant[2]);
            // Include subproperties of domain-bearing properties.
            std::vector<TermId> domain_all;
            for (TermId p : domain_props) {
                for (TermId sub : schema_->properties_entailing(p)) {
                    bool seen = false;
                    for (TermId existing : domain_all) {
                        if (existing == sub) {
                            seen = true;
                            break;
                        }
                    }
                    if (!seen) domain_all.push_back(sub);
                }
            }
            if (!domain_all.empty()) {
                arms.push_back(std::make_unique<InferredTypeScan>(
                    store_, schema_->type_id(), pattern.constant[2], std::move(domain_all),
                    InferredTypeScan::Side::Domain, pattern.slot[0], graph_constant_,
                    graph_slot_));
            }
            auto range_props = schema_->properties_with_range(pattern.constant[2]);
            std::vector<TermId> range_all;
            for (TermId p : range_props) {
                for (TermId sub : schema_->properties_entailing(p)) {
                    bool seen = false;
                    for (TermId existing : range_all) {
                        if (existing == sub) {
                            seen = true;
                            break;
                        }
                    }
                    if (!seen) range_all.push_back(sub);
                }
            }
            if (!range_all.empty()) {
                arms.push_back(std::make_unique<InferredTypeScan>(
                    store_, schema_->type_id(), pattern.constant[2], std::move(range_all),
                    InferredTypeScan::Side::Range, pattern.slot[0], graph_constant_,
                    graph_slot_));
            }
        } else if (pattern.constant[1].valid()) {
            for (TermId property : schema_->properties_entailing(pattern.constant[1])) {
                CompiledPattern alt = pattern;
                alt.constant[1] = property;
                arms.push_back(make_scan(std::move(alt), prebound));
            }
        } else {
            arms.push_back(make_scan(pattern, prebound));
        }

        if (arms.empty()) return make_scan(std::move(pattern), prebound);
        OperatorPtr chain = std::move(arms[0]);
        for (std::size_t i = 1; i < arms.size(); ++i) {
            chain = std::make_unique<UnionOperator>(std::move(chain), std::move(arms[i]));
        }
        if (arms.size() > 1) chain = std::make_unique<DistinctOperator>(std::move(chain));
        return chain;
    }

    OperatorPtr compile_bgp(const BgpNode& bgp) {
        if (bgp.patterns.empty()) return std::make_unique<UnitOperator>();

        std::vector<CompiledPattern> compiled;
        compiled.reserve(bgp.patterns.size());
        for (const TriplePattern& pattern : bgp.patterns) {
            compiled.push_back(compile_pattern(pattern));
        }
        std::vector<std::size_t> cardinalities;
        cardinalities.reserve(compiled.size());
        for (const CompiledPattern& pattern : compiled) {
            EncodedPattern encoded{pattern.constant[0], pattern.constant[1], pattern.constant[2]};
            cardinalities.push_back(pattern.impossible ? 0
                                                       : store_.count(encoded, graph_constant_));
        }
        std::vector<std::size_t> order = order_patterns(compiled, cardinalities);

        for (std::size_t index : order) {
            estimate_ = estimate_ == 0 ? cardinalities[index]
                                       : estimate_ * std::max<std::size_t>(cardinalities[index], 1);
        }

        std::vector<int> bound;
        pattern_variables(compiled[order[0]], bound);
        OperatorPtr chain = options_.rdfs_query_time
                                ? make_rdfs_scan(compiled[order[0]], {})
                                : make_scan(compiled[order[0]], {});

        for (std::size_t i = 1; i < order.size(); ++i) {
            const CompiledPattern& pattern = compiled[order[i]];
            if (options_.rdfs_query_time) {
                auto right = make_rdfs_scan(pattern, bound);
                chain = std::make_unique<NestedLoopJoin>(std::move(chain), std::move(right));
            } else {
                auto independent = make_scan(pattern, {});
                int left_sorted = chain->sorted_slot();
                bool merge_ok = options_.enable_merge_join && left_sorted >= 0 &&
                                left_sorted == independent->sorted_slot();
                if (merge_ok) {
                    chain = std::make_unique<MergeJoin>(std::move(chain), std::move(independent),
                                                        left_sorted);
                } else {
                    auto right = make_scan(pattern, bound);
                    chain = std::make_unique<NestedLoopJoin>(std::move(chain), std::move(right));
                }
            }
            pattern_variables(pattern, bound);
        }
        return chain;
    }

    OperatorPtr compile_graph(GraphNode& node) {
        TermId saved_constant = graph_constant_;
        int saved_slot = graph_slot_;

        OperatorPtr inner;
        if (node.graph.is_variable) {
            int slot = slot_of(node.graph.variable);
            graph_constant_ = kDefaultGraph;
            graph_slot_ = slot;
            inner = compile(*node.child);
            graph_constant_ = saved_constant;
            graph_slot_ = saved_slot;
            auto binder = std::make_unique<BindNamedGraphs>(store_, slot);
            return std::make_unique<NestedLoopJoin>(std::move(binder), std::move(inner));
        }

        TermId gid = store_.encode(node.graph.constant);
        graph_constant_ = gid.valid() ? gid : TermId{1};  // unknown name: empty index
        graph_slot_ = -1;
        if (!gid.valid()) {
            graph_constant_ = saved_constant;
            graph_slot_ = saved_slot;
            CompiledPattern empty;
            empty.impossible = true;
            empty.text = "GRAPH unknown";
            return std::make_unique<IndexScan>(store_, std::move(empty));
        }
        inner = compile(*node.child);
        graph_constant_ = saved_constant;
        graph_slot_ = saved_slot;
        return inner;
    }

    OperatorPtr compile(Algebra& algebra) {
        return std::visit(
            [&](auto& node) -> OperatorPtr {
                using T = std::decay_t<decltype(node)>;
                if constexpr (std::is_same_v<T, BgpNode>) {
                    return compile_bgp(node);
                } else if constexpr (std::is_same_v<T, JoinNode>) {
                    return std::make_unique<NestedLoopJoin>(compile(*node.left),
                                                            compile(*node.right));
                } else if constexpr (std::is_same_v<T, LeftJoinNode>) {
                    return std::make_unique<LeftJoin>(compile(*node.left), compile(*node.right),
                                                      node.condition.get(), store_.dictionary());
                } else if constexpr (std::is_same_v<T, UnionNode>) {
                    return std::make_unique<UnionOperator>(compile(*node.left),
                                                           compile(*node.right));
                } else if constexpr (std::is_same_v<T, FilterNode>) {
                    return std::make_unique<FilterOperator>(compile(*node.child),
                                                            node.condition.get(),
                                                            store_.dictionary());
                } else if constexpr (std::is_same_v<T, GraphNode>) {
                    return compile_graph(node);
                } else if constexpr (std::is_same_v<T, GroupNode>) {
                    std::vector<int> key_slots;
                    for (const std::string& key : node.keys) key_slots.push_back(slot_of(key));
                    std::vector<int> out_slots;
                    for (const Aggregate& agg : node.aggregates) {
                        out_slots.push_back(slot_of(agg.out_variable));
                    }
                    return std::make_unique<GroupOperator>(compile(*node.child),
                                                           std::move(key_slots), &node.aggregates,
                                                           std::move(out_slots),
                                                           store_.dictionary());
                } else if constexpr (std::is_same_v<T, ProjectNode>) {
                    std::vector<int> slots;
                    for (const std::string& name : node.variables) slots.push_back(slot_of(name));
                    return std::make_unique<ProjectOperator>(compile(*node.child),
                                                             std::move(slots));
                } else if constexpr (std::is_same_v<T, DistinctNode>) {
                    return std::make_unique<DistinctOperator>(compile(*node.child));
                } else if constexpr (std::is_same_v<T, OrderByNode>) {
                    return std::make_unique<SortOperator>(compile(*node.child), &node.keys,
                                                          store_.dictionary());
                } else {
                    return std::make_unique<SliceOperator>(compile(*node.child), node.offset,
                                                           node.limit);
                }
            },
            algebra.node);
    }

    TripleStore& store_;
    Query& query_;
    PlanOptions options_;
    std::vector<std::string> variables_;
    std::size_t estimate_ = 0;
    TermId graph_constant_ = kDefaultGraph;
    int graph_slot_ = -1;
    std::unique_ptr<RdfsSchema> schema_;
};

}  // namespace

Plan build_plan(TripleStore& store, Query& query, const PlanOptions& options) {
    return PlanBuilder(store, query, options).run();
}

}  // namespace trident
