// The execution engine: a pipeline of iterators over the indexes.
//
// Every operator writes into a row whose width is the number of variables in the
// query, so a binding lookup is an array index. open() takes the bindings the
// enclosing loop has already made, which is what turns a nested loop join into an
// index nested loop join: the scan below narrows its range with them.
#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "trident/expression.hpp"
#include "trident/sparql_ast.hpp"
#include "trident/store.hpp"

namespace trident {

class Operator {
public:
    virtual ~Operator() = default;
    // Starts a new sequence of solutions, extending the given bindings.
    virtual void open(const Row& input) = 0;
    virtual bool next(Row& out) = 0;
    // One line for the plan printout, without children.
    virtual std::string label() const = 0;
    virtual std::vector<const Operator*> children() const { return {}; }
    // The slot this operator emits in sorted order, or -1 when the output has no
    // useful order. Merge join is only legal above operators that answer this.
    virtual int sorted_slot() const { return -1; }

    std::size_t rows_produced() const { return rows_produced_; }

protected:
    std::size_t rows_produced_ = 0;
};

using OperatorPtr = std::unique_ptr<Operator>;

std::string describe_plan(const Operator& root, int indent = 0);

// A triple pattern compiled against the dictionary: every position is either a
// constant identifier or a slot number.
struct CompiledPattern {
    TermId constant[3] = {kUnbound, kUnbound, kUnbound};
    int slot[3] = {-1, -1, -1};
    bool impossible = false;  // a constant that the dictionary has never seen
    std::string text;         // the pattern as written, for the plan printout
};

// Scan of one triple pattern over the index that covers the most bound
// positions. Positions bound by the caller are folded into the search key, so
// the same class serves the outer scan and the inner side of a nested loop join.
class IndexScan : public Operator {
public:
    // prebound_slots are the variables the enclosing plan will have bound by the
    // time this scan is opened. They do not change the result, only which index
    // the scan will reach for, which is what the plan printout has to show.
    IndexScan(const TripleStore& store, CompiledPattern pattern,
              const std::vector<int>& prebound_slots = {});

    void open(const Row& input) override;
    bool next(Row& out) override;
    std::string label() const override;
    int sorted_slot() const override { return sorted_slot_; }

    // Exact number of triples the pattern matches with no outer bindings. This is
    // the statistic the planner orders joins by.
    std::size_t cardinality() const { return base_cardinality_; }
    const CompiledPattern& pattern() const { return pattern_; }

private:
    const TripleStore& store_;
    CompiledPattern pattern_;
    std::size_t base_cardinality_ = 0;
    int sorted_slot_ = -1;
    IndexOrder chosen_order_ = IndexOrder::Spo;
    int chosen_prefix_ = 0;

    Row input_;
    const PermutedIndex* index_ = nullptr;
    std::size_t cursor_ = 0, end_ = 0;
};

// Produces exactly one row, the empty solution mapping. This is the identity of
// join, and it is what an empty group pattern evaluates to.
class UnitOperator : public Operator {
public:
    void open(const Row& input) override;
    bool next(Row& out) override;
    std::string label() const override { return "Unit"; }

private:
    Row input_;
    bool done_ = true;
};

// Index nested loop join. For every row of the left input the right input is
// opened again with those bindings in place.
class NestedLoopJoin : public Operator {
public:
    NestedLoopJoin(OperatorPtr left, OperatorPtr right);
    void open(const Row& input) override;
    bool next(Row& out) override;
    std::string label() const override { return "IndexNestedLoopJoin"; }
    std::vector<const Operator*> children() const override { return {left_.get(), right_.get()}; }

private:
    OperatorPtr left_, right_;
    Row current_left_;
    bool have_left_ = false;
};

// Merge join over two inputs that are both sorted on the join variable. The
// planner only builds this when both sides report the same sorted slot.
class MergeJoin : public Operator {
public:
    MergeJoin(OperatorPtr left, OperatorPtr right, int join_slot);
    void open(const Row& input) override;
    bool next(Row& out) override;
    std::string label() const override;
    std::vector<const Operator*> children() const override { return {left_.get(), right_.get()}; }
    int sorted_slot() const override { return join_slot_; }

private:
    bool advance_left();
    bool advance_right();

    OperatorPtr left_, right_;
    int join_slot_;
    Row left_row_, right_row_;
    bool left_valid_ = false, right_valid_ = false;
    std::vector<Row> right_block_;  // the run of right rows sharing one key
    std::size_t block_cursor_ = 0;
    bool block_active_ = false;
};

class FilterOperator : public Operator {
public:
    FilterOperator(OperatorPtr child, const Expr* condition, const Dictionary& dictionary);
    void open(const Row& input) override;
    bool next(Row& out) override;
    std::string label() const override;
    std::vector<const Operator*> children() const override { return {child_.get()}; }
    int sorted_slot() const override { return child_->sorted_slot(); }

private:
    OperatorPtr child_;
    const Expr* condition_;
    const Dictionary& dictionary_;
};

class LeftJoin : public Operator {
public:
    LeftJoin(OperatorPtr left, OperatorPtr right, const Expr* condition,
             const Dictionary& dictionary);
    void open(const Row& input) override;
    bool next(Row& out) override;
    std::string label() const override;
    std::vector<const Operator*> children() const override { return {left_.get(), right_.get()}; }

private:
    OperatorPtr left_, right_;
    const Expr* condition_;
    const Dictionary& dictionary_;
    Row current_left_;
    bool have_left_ = false, matched_ = false;
};

class UnionOperator : public Operator {
public:
    UnionOperator(OperatorPtr left, OperatorPtr right);
    void open(const Row& input) override;
    bool next(Row& out) override;
    std::string label() const override { return "Union"; }
    std::vector<const Operator*> children() const override { return {left_.get(), right_.get()}; }

private:
    OperatorPtr left_, right_;
    Row input_;
    bool on_left_ = true;
};

// Keeps the projected slots and clears the rest, so that DISTINCT and the result
// writer both see exactly the columns the query asked for.
class ProjectOperator : public Operator {
public:
    ProjectOperator(OperatorPtr child, std::vector<int> slots);
    void open(const Row& input) override;
    bool next(Row& out) override;
    std::string label() const override { return "Project"; }
    std::vector<const Operator*> children() const override { return {child_.get()}; }

private:
    OperatorPtr child_;
    std::vector<int> slots_;
};

class DistinctOperator : public Operator {
public:
    explicit DistinctOperator(OperatorPtr child);
    void open(const Row& input) override;
    bool next(Row& out) override;
    std::string label() const override { return "Distinct"; }
    std::vector<const Operator*> children() const override { return {child_.get()}; }

private:
    OperatorPtr child_;
    std::unordered_set<std::string> seen_;
};

// Materialises its input. Sorting cannot be done any other way, and the plan
// printout says so.
class SortOperator : public Operator {
public:
    SortOperator(OperatorPtr child, const std::vector<OrderKey>* keys,
                 const Dictionary& dictionary);
    void open(const Row& input) override;
    bool next(Row& out) override;
    std::string label() const override { return "OrderBy (materialising)"; }
    std::vector<const Operator*> children() const override { return {child_.get()}; }

private:
    OperatorPtr child_;
    const std::vector<OrderKey>* keys_;
    const Dictionary& dictionary_;
    std::vector<Row> buffer_;
    std::size_t cursor_ = 0;
};

class SliceOperator : public Operator {
public:
    SliceOperator(OperatorPtr child, long long offset, long long limit);
    void open(const Row& input) override;
    bool next(Row& out) override;
    std::string label() const override;
    std::vector<const Operator*> children() const override { return {child_.get()}; }

private:
    OperatorPtr child_;
    long long offset_, limit_, emitted_ = 0;
};

// Grouping and aggregation. Materialising is inherent: no group is complete
// before the input is exhausted.
class GroupOperator : public Operator {
public:
    GroupOperator(OperatorPtr child, std::vector<int> key_slots,
                  const std::vector<Aggregate>* aggregates, std::vector<int> out_slots,
                  Dictionary& dictionary);
    void open(const Row& input) override;
    bool next(Row& out) override;
    std::string label() const override { return "GroupAggregate (materialising)"; }
    std::vector<const Operator*> children() const override { return {child_.get()}; }

private:
    OperatorPtr child_;
    std::vector<int> key_slots_;
    const std::vector<Aggregate>* aggregates_;
    std::vector<int> out_slots_;
    Dictionary& dictionary_;
    std::vector<Row> results_;
    std::size_t cursor_ = 0;
};

}  // namespace trident
