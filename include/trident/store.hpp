// Encoded triples and the three permuted indexes over them.
#pragma once

#include <array>
#include <cstddef>
#include <ostream>
#include <vector>

#include "trident/dictionary.hpp"
#include "trident/term.hpp"

namespace trident {

struct Triple {
    TermId s, p, o;

    constexpr TermId operator[](int component) const {
        return component == 0 ? s : (component == 1 ? p : o);
    }
    bool operator==(const Triple&) const = default;
};

// A triple pattern with constants already encoded. An absent component is a
// variable as far as the index is concerned.
struct EncodedPattern {
    TermId s = kUnbound, p = kUnbound, o = kUnbound;

    constexpr TermId operator[](int component) const {
        return component == 0 ? s : (component == 1 ? p : o);
    }
    constexpr bool bound(int component) const { return (*this)[component].valid(); }
};

enum class IndexOrder { Spo, Pos, Osp };

const char* index_name(IndexOrder order);

// One sorted permutation of the whole triple set. The permutation is the
// component order: SPO is {0,1,2}, POS is {1,2,0}, OSP is {2,0,1}.
class PermutedIndex {
public:
    explicit PermutedIndex(IndexOrder order);

    // Sorts a copy of the triple set into this permutation and drops duplicates.
    void assign(std::vector<Triple> triples);

    IndexOrder order() const { return order_; }
    const std::array<int, 3>& permutation() const { return perm_; }
    std::size_t size() const { return data_.size(); }
    const Triple& operator[](std::size_t i) const { return data_[i]; }

    // Half open range of triples whose first prefix_len components under this
    // permutation equal those of key.
    struct Range {
        std::size_t begin = 0, end = 0;
        std::size_t count() const { return end - begin; }
        bool empty() const { return begin >= end; }
    };
    Range prefix_range(const Triple& key, int prefix_len) const;

    std::size_t memory_bytes() const { return data_.capacity() * sizeof(Triple); }

private:
    IndexOrder order_;
    std::array<int, 3> perm_{};
    std::vector<Triple> data_;
};

// How many leading components of a pattern an index can use. Pattern (?s, p, o)
// has a prefix of two under POS and of zero under SPO.
struct IndexChoice {
    IndexOrder order = IndexOrder::Spo;
    int prefix_len = 0;
};

// Picks the index whose sorted prefix covers the most bound components. Ties
// break towards SPO, then POS, then OSP, so plan output is stable across runs.
IndexChoice choose_index(const EncodedPattern& pattern);

class TripleStore {
public:
    Dictionary& dictionary() { return dict_; }
    const Dictionary& dictionary() const { return dict_; }

    // Stages an already encoded triple. Duplicates are removed by build().
    void add(const Triple& t);
    // Encodes and stages. Convenience for the loader and for tests.
    void add(const Term& s, const Term& p, const Term& o);

    // Sorts the three indexes and drops duplicates. Must be called before any
    // scan; scanning a store that was modified since throws.
    void build();
    bool built() const { return built_; }

    std::size_t triple_count() const;
    std::size_t staged_count() const { return staging_.size(); }

    const PermutedIndex& index(IndexOrder order) const;

    // Exact number of triples matching the pattern. This is a binary search on
    // the chosen index, not a sample, so the planner works from counts that are
    // true rather than assumed.
    std::size_t count(const EncodedPattern& pattern) const;

    // Encodes a term. A term the dictionary has never seen yields kUnbound, so
    // callers must tell a variable apart from an unknown constant themselves.
    TermId encode(const Term& term) const { return dict_.lookup(term); }

    void write_ntriples(std::ostream& out) const;

    std::size_t memory_bytes() const;

private:
    Dictionary dict_;
    std::vector<Triple> staging_;
    PermutedIndex spo_{IndexOrder::Spo};
    PermutedIndex pos_{IndexOrder::Pos};
    PermutedIndex osp_{IndexOrder::Osp};
    bool built_ = false;
};

}  // namespace trident
