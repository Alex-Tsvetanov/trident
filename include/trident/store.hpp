// Encoded triples, named-graph quads, and the three permuted indexes over them.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <ostream>
#include <string>
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

// A triple in a named graph. The default graph uses kDefaultGraph as the name,
// which is the zero identifier and therefore cannot collide with any interned
// term.
struct Quad {
    TermId s, p, o, g;
    bool operator==(const Quad&) const = default;
};

// The name of the SPARQL default graph inside the store. It is not an RDF term.
inline constexpr TermId kDefaultGraph{};

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

// Owns a read-only mapping of a file. The store keeps one of these alive for as
// long as any index view points into it.
class MappedFile {
public:
    MappedFile() = default;
    explicit MappedFile(const std::string& path);
    ~MappedFile();

    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;
    MappedFile(MappedFile&& other) noexcept;
    MappedFile& operator=(MappedFile&& other) noexcept;

    const std::uint8_t* data() const { return data_; }
    std::size_t size() const { return size_; }
    explicit operator bool() const { return data_ != nullptr; }

private:
    int fd_ = -1;
    std::uint8_t* data_ = nullptr;
    std::size_t size_ = 0;
};

// One sorted permutation of a triple set. The permutation is the component
// order: SPO is {0,1,2}, POS is {1,2,0}, OSP is {2,0,1}. The rows may be owned
// or a view into a memory-mapped file; both look the same to a scan.
class PermutedIndex {
public:
    explicit PermutedIndex(IndexOrder order);

    // Sorts a copy of the triple set into this permutation and drops duplicates.
    void assign(std::vector<Triple> triples);
    // Points at an already sorted, deduplicated region. The caller keeps the
    // bytes alive for the lifetime of this index.
    void assign_view(const Triple* data, std::size_t count);

    IndexOrder order() const { return order_; }
    const std::array<int, 3>& permutation() const { return perm_; }
    std::size_t size() const { return view_ ? view_count_ : data_.size(); }
    const Triple& operator[](std::size_t i) const {
        return view_ ? view_[i] : data_[i];
    }
    bool is_mapped() const { return view_ != nullptr; }

    // Half open range of triples whose first prefix_len components under this
    // permutation equal those of key.
    struct Range {
        std::size_t begin = 0, end = 0;
        std::size_t count() const { return end - begin; }
        bool empty() const { return begin >= end; }
    };
    Range prefix_range(const Triple& key, int prefix_len) const;

    std::size_t memory_bytes() const {
        return view_ ? view_count_ * sizeof(Triple) : data_.capacity() * sizeof(Triple);
    }

private:
    IndexOrder order_;
    std::array<int, 3> perm_{};
    std::vector<Triple> data_;
    const Triple* view_ = nullptr;
    std::size_t view_count_ = 0;
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

// On-disk layout for TripleStore::save / TripleStore::open.
enum class StoreCodec {
    // Three raw Triple arrays after the dictionary. open() memory-maps them.
    Plain = 0,
    // Delta + variable-byte compression of each index. open() decompresses into
    // owned vectors; the dictionary is still read from the mapping.
    Compressed = 1,
};

struct SaveOptions {
    StoreCodec codec = StoreCodec::Compressed;
};

class TripleStore {
public:
    TripleStore() = default;
    TripleStore(const TripleStore&) = delete;
    TripleStore& operator=(const TripleStore&) = delete;
    TripleStore(TripleStore&&) noexcept = default;
    TripleStore& operator=(TripleStore&&) noexcept = default;

    Dictionary& dictionary() { return dict_; }
    const Dictionary& dictionary() const { return dict_; }

    // Stages an already encoded triple in the default graph. Duplicates are
    // removed by build().
    void add(const Triple& t);
    // Encodes and stages in the default graph.
    void add(const Term& s, const Term& p, const Term& o);
    // Stages a quad. A default-graph quad is the same as add(triple).
    void add(const Quad& q);
    void add(const Term& s, const Term& p, const Term& o, const Term& g);

    // Sorts the indexes and drops duplicates. Must be called before any scan;
    // scanning a store that was modified since throws.
    void build();
    bool built() const { return built_; }

    std::size_t triple_count() const;
    std::size_t quad_count() const;
    std::size_t staged_count() const { return staging_.size(); }
    std::size_t named_graph_count() const { return named_.size(); }
    // Names of every named graph that has at least one quad after build().
    std::vector<TermId> named_graphs() const;

    const PermutedIndex& index(IndexOrder order) const;
    // Indexes of one named graph. Throws when the name is unknown.
    const PermutedIndex& index(IndexOrder order, TermId graph) const;

    // Exact number of triples matching the pattern in the default graph.
    std::size_t count(const EncodedPattern& pattern) const;
    // Exact count inside one graph. Pass kDefaultGraph for the default graph.
    std::size_t count(const EncodedPattern& pattern, TermId graph) const;

    TermId encode(const Term& term) const { return dict_.lookup(term); }

    void write_ntriples(std::ostream& out) const;
    void write_nquads(std::ostream& out) const;

    std::size_t memory_bytes() const;

    // Writes the dictionary and every index to path. The default codec compresses
    // the indexes; StoreCodec::Plain leaves them as raw triples so open() can
    // memory-map them without a copy.
    void save(const std::string& path, const SaveOptions& options = {}) const;

    // Reopens a file written by save(). Plain indexes are used through a
    // memory map; compressed indexes are decoded into vectors. Neither path
    // rebuilds or re-sorts the indexes.
    static TripleStore open(const std::string& path);

private:
    struct NamedGraph {
        TermId name;
        std::vector<Triple> staging;
        PermutedIndex spo{IndexOrder::Spo};
        PermutedIndex pos{IndexOrder::Pos};
        PermutedIndex osp{IndexOrder::Osp};
    };

    NamedGraph& ensure_named(TermId graph);
    const NamedGraph* find_named(TermId graph) const;
    void require_built() const;
    // Copies any mmap-backed index into owned storage before the mapping is
    // released. Called from the mutators that invalidate a mapped store.
    void detach_mapping();

    Dictionary dict_;
    std::vector<Triple> staging_;
    PermutedIndex spo_{IndexOrder::Spo};
    PermutedIndex pos_{IndexOrder::Pos};
    PermutedIndex osp_{IndexOrder::Osp};
    std::vector<NamedGraph> named_;
    bool built_ = false;
    // Keeps mmap bytes alive for index views. Empty when the store owns its data.
    std::shared_ptr<MappedFile> mapping_;
};

}  // namespace trident
