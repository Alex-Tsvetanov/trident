#include "trident/exec.hpp"

#include <algorithm>
#include <cstring>
#include <sstream>
#include <stdexcept>

namespace trident {

namespace {

// A byte key for a row, used where rows have to be compared for equality in a
// hash table. Identifiers are fixed width, so the key is just their bytes.
std::string row_key(const Row& row, const std::vector<int>& slots) {
    std::string key;
    key.resize(slots.size() * sizeof(std::uint64_t));
    for (std::size_t i = 0; i < slots.size(); ++i) {
        std::uint64_t raw = slots[i] >= 0 ? row[static_cast<std::size_t>(slots[i])].raw() : 0;
        std::memcpy(key.data() + i * sizeof(raw), &raw, sizeof(raw));
    }
    return key;
}

std::string whole_row_key(const Row& row) {
    std::string key;
    key.resize(row.size() * sizeof(std::uint64_t));
    for (std::size_t i = 0; i < row.size(); ++i) {
        std::uint64_t raw = row[i].raw();
        std::memcpy(key.data() + i * sizeof(raw), &raw, sizeof(raw));
    }
    return key;
}

}  // namespace

std::string describe_plan(const Operator& root, int indent) {
    std::string out(static_cast<std::size_t>(indent) * 2, ' ');
    out += root.label();
    out += "\n";
    for (const Operator* child : root.children()) {
        if (child) out += describe_plan(*child, indent + 1);
    }
    return out;
}

// --- IndexScan -----------------------------------------------------------

IndexScan::IndexScan(const TripleStore& store, CompiledPattern pattern,
                     const std::vector<int>& prebound_slots, TermId graph, int graph_slot)
    : store_(store), pattern_(std::move(pattern)), graph_(graph), graph_slot_(graph_slot) {
    EncodedPattern base;
    base.s = pattern_.constant[0];
    base.p = pattern_.constant[1];
    base.o = pattern_.constant[2];
    if (pattern_.impossible) {
        base_cardinality_ = 0;
    } else if (graph_slot_ >= 0) {
        // Bound at open time; use the default-graph count as a stable underestimate.
        base_cardinality_ = store_.count(base);
    } else {
        base_cardinality_ = store_.count(base, graph_);
    }

    EncodedPattern expected = base;
    TermId placeholder{1};
    for (int i = 0; i < 3; ++i) {
        if (pattern_.slot[i] < 0) continue;
        if (std::find(prebound_slots.begin(), prebound_slots.end(), pattern_.slot[i]) ==
            prebound_slots.end()) {
            continue;
        }
        (i == 0 ? expected.s : (i == 1 ? expected.p : expected.o)) = placeholder;
    }
    IndexChoice choice = choose_index(expected);
    chosen_order_ = choice.order;
    chosen_prefix_ = choice.prefix_len;
    const std::array<int, 3>& perm = store_.index(choice.order).permutation();
    if (choice.prefix_len < 3) {
        int component = perm[static_cast<std::size_t>(choice.prefix_len)];
        sorted_slot_ = pattern_.slot[component];
    }
}

void IndexScan::open(const Row& input) {
    input_ = input;
    rows_produced_ = 0;
    cursor_ = end_ = 0;
    index_ = nullptr;
    if (pattern_.impossible) return;

    TermId graph = graph_;
    if (graph_slot_ >= 0) {
        graph = input_[static_cast<std::size_t>(graph_slot_)];
        if (!graph.valid()) return;
    }

    EncodedPattern effective;
    TermId parts[3];
    for (int i = 0; i < 3; ++i) {
        parts[i] = pattern_.constant[i];
        if (!parts[i].valid() && pattern_.slot[i] >= 0) {
            TermId bound = input_[static_cast<std::size_t>(pattern_.slot[i])];
            if (bound.valid()) parts[i] = bound;
        }
    }
    effective.s = parts[0];
    effective.p = parts[1];
    effective.o = parts[2];

    IndexChoice choice = choose_index(effective);
    try {
        index_ = &store_.index(choice.order, graph);
    } catch (const std::out_of_range&) {
        index_ = nullptr;
        return;
    }
    PermutedIndex::Range range =
        index_->prefix_range(Triple{effective.s, effective.p, effective.o}, choice.prefix_len);
    cursor_ = range.begin;
    end_ = range.end;
}

bool IndexScan::next(Row& out) {
    if (!index_) return false;
    while (cursor_ < end_) {
        const Triple& triple = (*index_)[cursor_++];
        out = input_;
        bool ok = true;
        for (int i = 0; i < 3 && ok; ++i) {
            int slot = pattern_.slot[i];
            if (slot < 0) continue;
            TermId value = triple[i];
            TermId existing = out[static_cast<std::size_t>(slot)];
            if (existing.valid()) {
                // The same variable in two positions of one pattern, or already
                // bound by the caller.
                if (existing != value) ok = false;
            } else {
                out[static_cast<std::size_t>(slot)] = value;
            }
        }
        if (ok) {
            ++rows_produced_;
            return true;
        }
    }
    return false;
}

std::string IndexScan::label() const {
    std::ostringstream out;
    out << "IndexScan " << index_name(chosen_order_) << " prefix=" << chosen_prefix_
        << " card=" << base_cardinality_ << "  { " << pattern_.text << " }";
    return out.str();
}

// --- BindNamedGraphs -----------------------------------------------------

BindNamedGraphs::BindNamedGraphs(const TripleStore& store, int graph_slot)
    : store_(store), graph_slot_(graph_slot) {}

void BindNamedGraphs::open(const Row& input) {
    input_ = input;
    rows_produced_ = 0;
    graphs_ = store_.named_graphs();
    cursor_ = 0;
}

bool BindNamedGraphs::next(Row& out) {
    if (cursor_ >= graphs_.size()) return false;
    out = input_;
    out[static_cast<std::size_t>(graph_slot_)] = graphs_[cursor_++];
    ++rows_produced_;
    return true;
}

std::string BindNamedGraphs::label() const {
    return "BindNamedGraphs graphs=" + std::to_string(store_.named_graph_count());
}

// --- InferredTypeScan ----------------------------------------------------

InferredTypeScan::InferredTypeScan(const TripleStore& store, TermId type_pred, TermId klass,
                                   std::vector<TermId> properties, Side side, int subject_slot,
                                   TermId graph, int graph_slot)
    : store_(store), type_pred_(type_pred), klass_(klass), properties_(std::move(properties)),
      side_(side), subject_slot_(subject_slot), graph_(graph), graph_slot_(graph_slot) {}

void InferredTypeScan::open(const Row& input) {
    input_ = input;
    rows_produced_ = 0;
    prop_index_ = 0;
    cursor_ = end_ = 0;
    index_ = nullptr;
    seen_.clear();
}

bool InferredTypeScan::next(Row& out) {
    TermId graph = graph_;
    if (graph_slot_ >= 0) {
        graph = input_[static_cast<std::size_t>(graph_slot_)];
        if (!graph.valid()) return false;
    }

    while (prop_index_ <= properties_.size()) {
        if (!index_ || cursor_ >= end_) {
            if (prop_index_ >= properties_.size()) return false;
            TermId property = properties_[prop_index_++];
            EncodedPattern key;
            key.p = property;
            IndexChoice choice = choose_index(key);
            try {
                index_ = &store_.index(choice.order, graph);
            } catch (const std::out_of_range&) {
                index_ = nullptr;
                continue;
            }
            auto range = index_->prefix_range(Triple{key.s, key.p, key.o}, choice.prefix_len);
            cursor_ = range.begin;
            end_ = range.end;
            continue;
        }
        const Triple& triple = (*index_)[cursor_++];
        TermId resource = side_ == Side::Domain ? triple.s : triple.o;
        if (!seen_.insert(resource.raw()).second) continue;
        out = input_;
        if (subject_slot_ >= 0) {
            TermId existing = out[static_cast<std::size_t>(subject_slot_)];
            if (existing.valid() && existing != resource) continue;
            out[static_cast<std::size_t>(subject_slot_)] = resource;
        }
        ++rows_produced_;
        return true;
    }
    return false;
}

std::string InferredTypeScan::label() const {
    return std::string("InferredTypeScan ") + (side_ == Side::Domain ? "domain" : "range") +
           " props=" + std::to_string(properties_.size());
}

// --- UnitOperator --------------------------------------------------------

void UnitOperator::open(const Row& input) {
    rows_produced_ = 0;
    input_ = input;
    done_ = false;
}

bool UnitOperator::next(Row& out) {
    if (done_) return false;
    done_ = true;
    out = input_;
    ++rows_produced_;
    return true;
}

// --- NestedLoopJoin ------------------------------------------------------

NestedLoopJoin::NestedLoopJoin(OperatorPtr left, OperatorPtr right)
    : left_(std::move(left)), right_(std::move(right)) {}

void NestedLoopJoin::open(const Row& input) {
    rows_produced_ = 0;
    have_left_ = false;
    left_->open(input);
}

bool NestedLoopJoin::next(Row& out) {
    for (;;) {
        if (!have_left_) {
            if (!left_->next(current_left_)) return false;
            have_left_ = true;
            right_->open(current_left_);
        }
        if (right_->next(out)) {
            ++rows_produced_;
            return true;
        }
        have_left_ = false;
    }
}

// --- MergeJoin -----------------------------------------------------------

MergeJoin::MergeJoin(OperatorPtr left, OperatorPtr right, int join_slot)
    : left_(std::move(left)), right_(std::move(right)), join_slot_(join_slot) {}

void MergeJoin::open(const Row& input) {
    rows_produced_ = 0;
    left_->open(input);
    right_->open(input);
    left_valid_ = advance_left();
    right_valid_ = advance_right();
    right_block_.clear();
    block_cursor_ = 0;
    block_active_ = false;
}

bool MergeJoin::advance_left() { return left_->next(left_row_); }
bool MergeJoin::advance_right() { return right_->next(right_row_); }

bool MergeJoin::next(Row& out) {
    const std::size_t slot = static_cast<std::size_t>(join_slot_);
    for (;;) {
        if (block_active_) {
            if (block_cursor_ < right_block_.size()) {
                const Row& right = right_block_[block_cursor_++];
                out = left_row_;
                for (std::size_t i = 0; i < out.size(); ++i) {
                    if (!out[i].valid()) out[i] = right[i];
                }
                ++rows_produced_;
                return true;
            }
            // The block is exhausted for this left row: take the next left row and
            // replay the block while the key still matches.
            left_valid_ = advance_left();
            block_cursor_ = 0;
            if (!left_valid_ || right_block_.empty() ||
                left_row_[slot] != right_block_.front()[slot]) {
                block_active_ = false;
                right_block_.clear();
            }
            continue;
        }
        if (!left_valid_ || !right_valid_) return false;
        TermId lk = left_row_[slot];
        TermId rk = right_row_[slot];
        if (lk < rk) {
            left_valid_ = advance_left();
            continue;
        }
        if (rk < lk) {
            right_valid_ = advance_right();
            continue;
        }
        // Equal keys: buffer the whole run on the right.
        right_block_.clear();
        while (right_valid_ && right_row_[slot] == lk) {
            right_block_.push_back(right_row_);
            right_valid_ = advance_right();
        }
        block_cursor_ = 0;
        block_active_ = true;
    }
}

std::string MergeJoin::label() const {
    return "MergeJoin on slot " + std::to_string(join_slot_);
}

// --- FilterOperator ------------------------------------------------------

FilterOperator::FilterOperator(OperatorPtr child, const Expr* condition,
                               const Dictionary& dictionary)
    : child_(std::move(child)), condition_(condition), dictionary_(dictionary) {}

void FilterOperator::open(const Row& input) {
    rows_produced_ = 0;
    child_->open(input);
}

bool FilterOperator::next(Row& out) {
    while (child_->next(out)) {
        if (effective_boolean(evaluate(*condition_, out, dictionary_))) {
            ++rows_produced_;
            return true;
        }
    }
    return false;
}

std::string FilterOperator::label() const {
    return "Filter " + expr_to_string(*condition_);
}

// --- LeftJoin ------------------------------------------------------------

LeftJoin::LeftJoin(OperatorPtr left, OperatorPtr right, const Expr* condition,
                   const Dictionary& dictionary)
    : left_(std::move(left)), right_(std::move(right)), condition_(condition),
      dictionary_(dictionary) {}

void LeftJoin::open(const Row& input) {
    rows_produced_ = 0;
    have_left_ = false;
    matched_ = false;
    left_->open(input);
}

bool LeftJoin::next(Row& out) {
    for (;;) {
        if (!have_left_) {
            if (!left_->next(current_left_)) return false;
            have_left_ = true;
            matched_ = false;
            right_->open(current_left_);
        }
        Row candidate;
        while (right_->next(candidate)) {
            if (condition_ && !effective_boolean(evaluate(*condition_, candidate, dictionary_))) {
                continue;
            }
            matched_ = true;
            out = candidate;
            ++rows_produced_;
            return true;
        }
        have_left_ = false;
        if (!matched_) {
            // No compatible solution on the right: the left row survives with the
            // right hand variables unbound.
            out = current_left_;
            ++rows_produced_;
            return true;
        }
    }
}

std::string LeftJoin::label() const {
    std::string out = "LeftJoin";
    if (condition_) out += " [" + expr_to_string(*condition_) + "]";
    return out;
}

// --- UnionOperator -------------------------------------------------------

UnionOperator::UnionOperator(OperatorPtr left, OperatorPtr right)
    : left_(std::move(left)), right_(std::move(right)) {}

void UnionOperator::open(const Row& input) {
    rows_produced_ = 0;
    input_ = input;
    on_left_ = true;
    left_->open(input_);
}

bool UnionOperator::next(Row& out) {
    if (on_left_) {
        if (left_->next(out)) {
            ++rows_produced_;
            return true;
        }
        on_left_ = false;
        right_->open(input_);
    }
    if (right_->next(out)) {
        ++rows_produced_;
        return true;
    }
    return false;
}

// --- ProjectOperator -----------------------------------------------------

ProjectOperator::ProjectOperator(OperatorPtr child, std::vector<int> slots)
    : child_(std::move(child)), slots_(std::move(slots)) {}

void ProjectOperator::open(const Row& input) {
    rows_produced_ = 0;
    child_->open(input);
}

bool ProjectOperator::next(Row& out) {
    Row wide;
    if (!child_->next(wide)) return false;
    out.assign(wide.size(), kUnbound);
    for (int slot : slots_) {
        if (slot >= 0) out[static_cast<std::size_t>(slot)] = wide[static_cast<std::size_t>(slot)];
    }
    ++rows_produced_;
    return true;
}

// --- DistinctOperator ----------------------------------------------------

DistinctOperator::DistinctOperator(OperatorPtr child) : child_(std::move(child)) {}

void DistinctOperator::open(const Row& input) {
    rows_produced_ = 0;
    seen_.clear();
    child_->open(input);
}

bool DistinctOperator::next(Row& out) {
    while (child_->next(out)) {
        if (seen_.insert(whole_row_key(out)).second) {
            ++rows_produced_;
            return true;
        }
    }
    return false;
}

// --- SortOperator --------------------------------------------------------

SortOperator::SortOperator(OperatorPtr child, const std::vector<OrderKey>* keys,
                           const Dictionary& dictionary)
    : child_(std::move(child)), keys_(keys), dictionary_(dictionary) {}

void SortOperator::open(const Row& input) {
    rows_produced_ = 0;
    buffer_.clear();
    cursor_ = 0;
    child_->open(input);
    Row row;
    while (child_->next(row)) buffer_.push_back(row);

    // The sort keys are computed once per row rather than once per comparison.
    // Evaluating them inside the comparator costs a dictionary lookup and a term
    // copy on every one of the O(n log n) comparisons, which dominated the sort.
    const std::vector<OrderKey>& keys = *keys_;
    const std::size_t key_count = keys.size();
    std::vector<Value> computed(buffer_.size() * key_count);
    for (std::size_t r = 0; r < buffer_.size(); ++r) {
        for (std::size_t k = 0; k < key_count; ++k) {
            computed[r * key_count + k] = evaluate(*keys[k].expr, buffer_[r], dictionary_);
        }
    }

    std::vector<std::size_t> order(buffer_.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        for (std::size_t k = 0; k < key_count; ++k) {
            int cmp = compare_values(computed[a * key_count + k], computed[b * key_count + k]);
            if (cmp == 0) continue;
            return keys[k].descending ? cmp > 0 : cmp < 0;
        }
        return false;
    });

    std::vector<Row> sorted;
    sorted.reserve(buffer_.size());
    for (std::size_t index : order) sorted.push_back(std::move(buffer_[index]));
    buffer_ = std::move(sorted);
}

bool SortOperator::next(Row& out) {
    if (cursor_ >= buffer_.size()) return false;
    out = buffer_[cursor_++];
    ++rows_produced_;
    return true;
}

// --- SliceOperator -------------------------------------------------------

SliceOperator::SliceOperator(OperatorPtr child, long long offset, long long limit)
    : child_(std::move(child)), offset_(offset), limit_(limit) {}

void SliceOperator::open(const Row& input) {
    rows_produced_ = 0;
    emitted_ = 0;
    child_->open(input);
    Row skip;
    for (long long i = 0; i < offset_; ++i) {
        if (!child_->next(skip)) break;
    }
}

bool SliceOperator::next(Row& out) {
    if (limit_ >= 0 && emitted_ >= limit_) return false;
    if (!child_->next(out)) return false;
    ++emitted_;
    ++rows_produced_;
    return true;
}

std::string SliceOperator::label() const {
    return "Slice offset=" + std::to_string(offset_) + " limit=" +
           (limit_ < 0 ? std::string("none") : std::to_string(limit_));
}

// --- GroupOperator -------------------------------------------------------

namespace {

struct AggregateState {
    long long count = 0;
    double sum = 0;
    bool has_extreme = false;
    Term extreme;
    bool numeric_only = true;
    std::unordered_set<std::string> distinct_seen;
};

}  // namespace

GroupOperator::GroupOperator(OperatorPtr child, std::vector<int> key_slots,
                             const std::vector<Aggregate>* aggregates,
                             std::vector<int> out_slots, Dictionary& dictionary)
    : child_(std::move(child)), key_slots_(std::move(key_slots)), aggregates_(aggregates),
      out_slots_(std::move(out_slots)), dictionary_(dictionary) {}

void GroupOperator::open(const Row& input) {
    rows_produced_ = 0;
    results_.clear();
    cursor_ = 0;
    child_->open(input);

    const std::vector<Aggregate>& aggregates = *aggregates_;
    std::unordered_map<std::string, std::size_t> group_of;
    std::vector<Row> representatives;
    std::vector<std::vector<AggregateState>> states;

    // The seed row already has the full width of the query, so an empty input
    // still gives the aggregate row somewhere to write its result.
    const std::size_t width = input.size();
    Row row;
    while (child_->next(row)) {
        std::string key = row_key(row, key_slots_);
        auto it = group_of.find(key);
        std::size_t index;
        if (it == group_of.end()) {
            index = representatives.size();
            group_of.emplace(std::move(key), index);
            representatives.push_back(row);
            states.emplace_back(aggregates.size());
        } else {
            index = it->second;
        }
        std::vector<AggregateState>& group = states[index];
        for (std::size_t a = 0; a < aggregates.size(); ++a) {
            const Aggregate& agg = aggregates[a];
            AggregateState& state = group[a];
            if (agg.star) {
                ++state.count;
                continue;
            }
            Value value = evaluate(*agg.argument, row, dictionary_);
            if (!value.is_bound()) continue;
            if (agg.distinct && !state.distinct_seen.insert(value.term.to_ntriples()).second) {
                continue;
            }
            ++state.count;
            double number = 0;
            if (numeric_value(value.term, number)) {
                state.sum += number;
            } else {
                state.numeric_only = false;
            }
            if (!state.has_extreme) {
                state.has_extreme = true;
                state.extreme = value.term;
            } else {
                int cmp = compare_terms(value.term, state.extreme);
                if ((agg.func == AggregateFunc::Min && cmp < 0) ||
                    (agg.func == AggregateFunc::Max && cmp > 0)) {
                    state.extreme = value.term;
                }
            }
        }
    }

    if (representatives.empty() && key_slots_.empty()) {
        // An aggregate query with no grouping still returns one row, with COUNT
        // equal to zero. Nothing else can be reported for an empty input.
        representatives.push_back(Row(width, kUnbound));
        states.emplace_back(aggregates.size());
    }

    for (std::size_t g = 0; g < representatives.size(); ++g) {
        Row out(width, kUnbound);
        for (int slot : key_slots_) {
            if (slot >= 0) {
                out[static_cast<std::size_t>(slot)] =
                    representatives[g][static_cast<std::size_t>(slot)];
            }
        }
        for (std::size_t a = 0; a < aggregates.size(); ++a) {
            const Aggregate& agg = aggregates[a];
            const AggregateState& state = states[g][a];
            int slot = out_slots_[a];
            if (slot < 0) continue;
            Term result;
            bool have = true;
            switch (agg.func) {
                case AggregateFunc::Count:
                    result = Term::typed_literal(std::to_string(state.count),
                                                 std::string(xsd::kInteger));
                    break;
                case AggregateFunc::Sum:
                    result = Value::number(state.sum).term;
                    if (state.count == 0) {
                        result = Term::typed_literal("0", std::string(xsd::kInteger));
                    }
                    break;
                case AggregateFunc::Avg:
                    if (state.count == 0) {
                        have = false;
                    } else {
                        result = Value::number(state.sum / static_cast<double>(state.count)).term;
                    }
                    break;
                case AggregateFunc::Min:
                case AggregateFunc::Max:
                case AggregateFunc::SampleFirst:
                    have = state.has_extreme;
                    result = state.extreme;
                    break;
            }
            if (have) out[static_cast<std::size_t>(slot)] = dictionary_.intern(result);
        }
        results_.push_back(std::move(out));
    }
}

bool GroupOperator::next(Row& out) {
    if (cursor_ >= results_.size()) return false;
    out = results_[cursor_++];
    ++rows_produced_;
    return true;
}

}  // namespace trident
