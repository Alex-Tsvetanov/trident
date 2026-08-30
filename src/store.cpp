#include "trident/store.hpp"

#include "trident/codec.hpp"

#include <algorithm>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <stdexcept>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace trident {

namespace {

constexpr char kMagic[8] = {'T', 'R', 'I', 'D', 'E', 'N', 'T', '\1'};
constexpr std::uint32_t kFormatVersion = 2;

std::array<int, 3> permutation_of(IndexOrder order) {
    switch (order) {
        case IndexOrder::Spo: return {0, 1, 2};
        case IndexOrder::Pos: return {1, 2, 0};
        case IndexOrder::Osp: return {2, 0, 1};
    }
    return {0, 1, 2};
}

void write_u32(std::ostream& out, std::uint32_t value) {
    out.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

void write_u64(std::ostream& out, std::uint64_t value) {
    out.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

std::uint32_t read_u32(const std::uint8_t*& cursor, const std::uint8_t* end) {
    if (static_cast<std::size_t>(end - cursor) < sizeof(std::uint32_t)) {
        throw std::runtime_error("trident: truncated store file");
    }
    std::uint32_t value = 0;
    std::memcpy(&value, cursor, sizeof(value));
    cursor += sizeof(value);
    return value;
}

std::uint64_t read_u64(const std::uint8_t*& cursor, const std::uint8_t* end) {
    if (static_cast<std::size_t>(end - cursor) < sizeof(std::uint64_t)) {
        throw std::runtime_error("trident: truncated store file");
    }
    std::uint64_t value = 0;
    std::memcpy(&value, cursor, sizeof(value));
    cursor += sizeof(value);
    return value;
}

void write_bytes(std::ostream& out, const std::string& text) {
    write_u32(out, static_cast<std::uint32_t>(text.size()));
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

std::string read_bytes(const std::uint8_t*& cursor, const std::uint8_t* end) {
    std::uint32_t length = read_u32(cursor, end);
    if (static_cast<std::size_t>(end - cursor) < length) {
        throw std::runtime_error("trident: truncated string in store file");
    }
    std::string text(reinterpret_cast<const char*>(cursor), length);
    cursor += length;
    return text;
}

void write_term(std::ostream& out, const Term& term) {
    out.put(static_cast<char>(term.kind));
    write_bytes(out, term.value);
    write_bytes(out, term.language);
    write_bytes(out, term.datatype);
}

Term read_term(const std::uint8_t*& cursor, const std::uint8_t* end) {
    if (cursor >= end) throw std::runtime_error("trident: truncated term");
    Term term;
    term.kind = static_cast<TermKind>(*cursor++);
    term.value = read_bytes(cursor, end);
    term.language = read_bytes(cursor, end);
    term.datatype = read_bytes(cursor, end);
    return term;
}

void write_plain_index(std::ostream& out, const PermutedIndex& index) {
    // Pad so the triple payload is 8-byte aligned and can be mapped as Triple*.
    while ((static_cast<std::uint64_t>(out.tellp()) + sizeof(std::uint64_t)) % 8 != 0) {
        out.put('\0');
    }
    write_u64(out, index.size());
    for (std::size_t i = 0; i < index.size(); ++i) {
        const Triple& t = index[i];
        write_u64(out, t.s.raw());
        write_u64(out, t.p.raw());
        write_u64(out, t.o.raw());
    }
}

void write_compressed_index(std::ostream& out, const PermutedIndex& index) {
    std::vector<std::uint8_t> bytes = compress_index(index);
    write_u64(out, bytes.size());
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
}

void load_plain_index(PermutedIndex& index, const std::uint8_t*& cursor, const std::uint8_t* end,
                      const std::uint8_t* base, bool map_in_place) {
    while (((cursor - base) + static_cast<std::ptrdiff_t>(sizeof(std::uint64_t))) % 8 != 0) {
        if (cursor >= end) throw std::runtime_error("trident: truncated padding");
        ++cursor;
    }
    std::uint64_t count = read_u64(cursor, end);
    std::size_t bytes = static_cast<std::size_t>(count) * 3 * sizeof(std::uint64_t);
    if (static_cast<std::size_t>(end - cursor) < bytes) {
        throw std::runtime_error("trident: truncated plain index");
    }
    if (map_in_place) {
        // The file layout writes three u64 fields per triple, which matches
        // Triple's representation on every platform this project targets.
        static_assert(sizeof(Triple) == 24, "plain mmap layout assumes 24-byte Triple");
        static_assert(alignof(Triple) == alignof(std::uint64_t),
                      "plain mmap layout assumes Triple alignment matches u64");
        const auto* triples = reinterpret_cast<const Triple*>(cursor);
        index.assign_view(triples, static_cast<std::size_t>(count));
        cursor += bytes;
        return;
    }
    std::vector<Triple> triples;
    triples.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t i = 0; i < count; ++i) {
        TermId s(read_u64(cursor, end));
        TermId p(read_u64(cursor, end));
        TermId o(read_u64(cursor, end));
        triples.push_back(Triple{s, p, o});
    }
    // The bytes are already sorted; assign() sorts again, which is idempotent.
    index.assign(std::move(triples));
}

void load_compressed_index(PermutedIndex& index, const std::uint8_t*& cursor,
                           const std::uint8_t* end) {
    std::uint64_t nbytes = read_u64(cursor, end);
    if (static_cast<std::size_t>(end - cursor) < nbytes) {
        throw std::runtime_error("trident: truncated compressed index");
    }
    std::vector<Triple> triples =
        decompress_index(cursor, static_cast<std::size_t>(nbytes), index.permutation());
    cursor += nbytes;
    index.assign(std::move(triples));
}

}  // namespace

const char* index_name(IndexOrder order) {
    switch (order) {
        case IndexOrder::Spo: return "SPO";
        case IndexOrder::Pos: return "POS";
        case IndexOrder::Osp: return "OSP";
    }
    return "???";
}

MappedFile::MappedFile(const std::string& path) {
    fd_ = ::open(path.c_str(), O_RDONLY);
    if (fd_ < 0) throw std::runtime_error("trident: cannot open store file: " + path);
    struct stat st {};
    if (::fstat(fd_, &st) != 0) {
        ::close(fd_);
        fd_ = -1;
        throw std::runtime_error("trident: cannot stat store file: " + path);
    }
    size_ = static_cast<std::size_t>(st.st_size);
    if (size_ == 0) {
        ::close(fd_);
        fd_ = -1;
        throw std::runtime_error("trident: empty store file: " + path);
    }
    void* mapped = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd_, 0);
    if (mapped == MAP_FAILED) {
        ::close(fd_);
        fd_ = -1;
        throw std::runtime_error("trident: mmap failed for " + path);
    }
    data_ = static_cast<std::uint8_t*>(mapped);
}

MappedFile::~MappedFile() {
    if (data_) ::munmap(data_, size_);
    if (fd_ >= 0) ::close(fd_);
}

MappedFile::MappedFile(MappedFile&& other) noexcept
    : fd_(other.fd_), data_(other.data_), size_(other.size_) {
    other.fd_ = -1;
    other.data_ = nullptr;
    other.size_ = 0;
}

MappedFile& MappedFile::operator=(MappedFile&& other) noexcept {
    if (this != &other) {
        if (data_) ::munmap(data_, size_);
        if (fd_ >= 0) ::close(fd_);
        fd_ = other.fd_;
        data_ = other.data_;
        size_ = other.size_;
        other.fd_ = -1;
        other.data_ = nullptr;
        other.size_ = 0;
    }
    return *this;
}

PermutedIndex::PermutedIndex(IndexOrder order)
    : order_(order), perm_(permutation_of(order)) {}

void PermutedIndex::assign(std::vector<Triple> triples) {
    view_ = nullptr;
    view_count_ = 0;
    data_ = std::move(triples);
    const std::array<int, 3> perm = perm_;
    auto less = [perm](const Triple& a, const Triple& b) {
        for (int i = 0; i < 3; ++i) {
            if (a[perm[i]] != b[perm[i]]) return a[perm[i]] < b[perm[i]];
        }
        return false;
    };
    std::sort(data_.begin(), data_.end(), less);
    data_.erase(std::unique(data_.begin(), data_.end()), data_.end());
    data_.shrink_to_fit();
}

void PermutedIndex::assign_view(const Triple* data, std::size_t count) {
    data_.clear();
    data_.shrink_to_fit();
    view_ = data;
    view_count_ = count;
}

PermutedIndex::Range PermutedIndex::prefix_range(const Triple& key, int prefix_len) const {
    const Triple* first = view_ ? view_ : data_.data();
    const Triple* last = first + size();
    if (prefix_len <= 0) {
        return Range{0, size()};
    }
    const std::array<int, 3> perm = perm_;
    const int n = prefix_len;
    auto less = [perm, n](const Triple& a, const Triple& b) {
        for (int i = 0; i < n; ++i) {
            if (a[perm[i]] != b[perm[i]]) return a[perm[i]] < b[perm[i]];
        }
        return false;
    };
    auto lo = std::lower_bound(first, last, key, less);
    auto hi = std::upper_bound(lo, last, key, less);
    return Range{static_cast<std::size_t>(lo - first), static_cast<std::size_t>(hi - first)};
}

IndexChoice choose_index(const EncodedPattern& pattern) {
    const IndexOrder orders[3] = {IndexOrder::Spo, IndexOrder::Pos, IndexOrder::Osp};
    IndexChoice best;
    best.prefix_len = -1;
    for (IndexOrder order : orders) {
        const std::array<int, 3> perm = permutation_of(order);
        int len = 0;
        while (len < 3 && pattern.bound(perm[len])) ++len;
        if (len > best.prefix_len) {
            best.order = order;
            best.prefix_len = len;
        }
    }
    return best;
}

void TripleStore::detach_mapping() {
    if (!mapping_) return;
    auto materialize = [](PermutedIndex& index) {
        if (!index.is_mapped()) return;
        std::vector<Triple> copy;
        copy.reserve(index.size());
        for (std::size_t i = 0; i < index.size(); ++i) copy.push_back(index[i]);
        index.assign(std::move(copy));
    };
    materialize(spo_);
    materialize(pos_);
    materialize(osp_);
    for (NamedGraph& graph : named_) {
        materialize(graph.spo);
        materialize(graph.pos);
        materialize(graph.osp);
    }
    mapping_.reset();
}

void TripleStore::add(const Triple& t) {
    detach_mapping();
    staging_.push_back(t);
    built_ = false;
}

void TripleStore::add(const Term& s, const Term& p, const Term& o) {
    add(Triple{dict_.intern(s), dict_.intern(p), dict_.intern(o)});
}

void TripleStore::add(const Quad& q) {
    if (!q.g.valid()) {
        add(Triple{q.s, q.p, q.o});
        return;
    }
    detach_mapping();
    ensure_named(q.g).staging.push_back(Triple{q.s, q.p, q.o});
    built_ = false;
}

void TripleStore::add(const Term& s, const Term& p, const Term& o, const Term& g) {
    TermId gid = dict_.intern(g);
    add(Quad{dict_.intern(s), dict_.intern(p), dict_.intern(o), gid});
}

TripleStore::NamedGraph& TripleStore::ensure_named(TermId graph) {
    if (NamedGraph* existing = const_cast<NamedGraph*>(find_named(graph))) {
        return *existing;
    }
    named_.push_back(NamedGraph{graph, {}, PermutedIndex{IndexOrder::Spo},
                                PermutedIndex{IndexOrder::Pos}, PermutedIndex{IndexOrder::Osp}});
    return named_.back();
}

const TripleStore::NamedGraph* TripleStore::find_named(TermId graph) const {
    for (const NamedGraph& g : named_) {
        if (g.name == graph) return &g;
    }
    return nullptr;
}

void TripleStore::require_built() const {
    if (!built_) throw std::logic_error("trident: TripleStore::build() has not been called");
}

void TripleStore::build() {
    spo_.assign(staging_);
    pos_.assign(staging_);
    osp_.assign(staging_);
    staging_.clear();
    staging_.reserve(spo_.size());
    for (std::size_t i = 0; i < spo_.size(); ++i) staging_.push_back(spo_[i]);

    for (NamedGraph& graph : named_) {
        graph.spo.assign(graph.staging);
        graph.pos.assign(graph.staging);
        graph.osp.assign(graph.staging);
        graph.staging.clear();
        graph.staging.reserve(graph.spo.size());
        for (std::size_t i = 0; i < graph.spo.size(); ++i) {
            graph.staging.push_back(graph.spo[i]);
        }
    }
    built_ = true;
}

std::size_t TripleStore::triple_count() const { return spo_.size(); }

std::size_t TripleStore::quad_count() const {
    std::size_t total = spo_.size();
    for (const NamedGraph& graph : named_) total += graph.spo.size();
    return total;
}

std::vector<TermId> TripleStore::named_graphs() const {
    std::vector<TermId> names;
    names.reserve(named_.size());
    for (const NamedGraph& graph : named_) {
        if (graph.spo.size() > 0) names.push_back(graph.name);
    }
    return names;
}

const PermutedIndex& TripleStore::index(IndexOrder order) const {
    switch (order) {
        case IndexOrder::Spo: return spo_;
        case IndexOrder::Pos: return pos_;
        case IndexOrder::Osp: return osp_;
    }
    return spo_;
}

const PermutedIndex& TripleStore::index(IndexOrder order, TermId graph) const {
    if (!graph.valid()) return index(order);
    const NamedGraph* found = find_named(graph);
    if (!found) throw std::out_of_range("trident: unknown named graph");
    switch (order) {
        case IndexOrder::Spo: return found->spo;
        case IndexOrder::Pos: return found->pos;
        case IndexOrder::Osp: return found->osp;
    }
    return found->spo;
}

std::size_t TripleStore::count(const EncodedPattern& pattern) const {
    return count(pattern, kDefaultGraph);
}

std::size_t TripleStore::count(const EncodedPattern& pattern, TermId graph) const {
    require_built();
    if (graph.valid() && !find_named(graph)) return 0;
    IndexChoice choice = choose_index(pattern);
    Triple key{pattern.s, pattern.p, pattern.o};
    return index(choice.order, graph).prefix_range(key, choice.prefix_len).count();
}

void TripleStore::write_ntriples(std::ostream& out) const {
    for (std::size_t i = 0; i < spo_.size(); ++i) {
        const Triple& t = spo_[i];
        out << dict_.decode(t.s).to_ntriples() << ' '
            << dict_.decode(t.p).to_ntriples() << ' '
            << dict_.decode(t.o).to_ntriples() << " .\n";
    }
}

void TripleStore::write_nquads(std::ostream& out) const {
    write_ntriples(out);
    for (const NamedGraph& graph : named_) {
        const Term& gterm = dict_.decode(graph.name);
        for (std::size_t i = 0; i < graph.spo.size(); ++i) {
            const Triple& t = graph.spo[i];
            out << dict_.decode(t.s).to_ntriples() << ' '
                << dict_.decode(t.p).to_ntriples() << ' '
                << dict_.decode(t.o).to_ntriples() << ' '
                << gterm.to_ntriples() << " .\n";
        }
    }
}

std::size_t TripleStore::memory_bytes() const {
    std::size_t bytes = dict_.memory_bytes() + spo_.memory_bytes() + pos_.memory_bytes() +
                        osp_.memory_bytes() + staging_.capacity() * sizeof(Triple);
    for (const NamedGraph& graph : named_) {
        bytes += graph.spo.memory_bytes() + graph.pos.memory_bytes() + graph.osp.memory_bytes() +
                 graph.staging.capacity() * sizeof(Triple);
    }
    if (mapping_) bytes += mapping_->size();
    return bytes;
}

void TripleStore::save(const std::string& path, const SaveOptions& options) const {
    require_built();
    std::ofstream out(path, std::ios::binary);
    if (!out) throw std::runtime_error("trident: cannot write store file: " + path);

    out.write(kMagic, sizeof(kMagic));
    write_u32(out, kFormatVersion);
    write_u32(out, static_cast<std::uint32_t>(options.codec));
    write_u64(out, dict_.size());
    write_u64(out, spo_.size());
    write_u64(out, named_.size());

    for (const Term& term : dict_.terms()) write_term(out, term);

    auto write_graph_indexes = [&](const PermutedIndex& spo, const PermutedIndex& pos,
                                   const PermutedIndex& osp) {
        if (options.codec == StoreCodec::Plain) {
            write_plain_index(out, spo);
            write_plain_index(out, pos);
            write_plain_index(out, osp);
        } else {
            write_compressed_index(out, spo);
            write_compressed_index(out, pos);
            write_compressed_index(out, osp);
        }
    };

    write_graph_indexes(spo_, pos_, osp_);
    for (const NamedGraph& graph : named_) {
        write_u64(out, graph.name.raw());
        write_graph_indexes(graph.spo, graph.pos, graph.osp);
    }
    if (!out) throw std::runtime_error("trident: failed while writing store file: " + path);
}

TripleStore TripleStore::open(const std::string& path) {
    auto mapping = std::make_shared<MappedFile>(path);
    const std::uint8_t* cursor = mapping->data();
    const std::uint8_t* end = mapping->data() + mapping->size();

    if (static_cast<std::size_t>(end - cursor) < sizeof(kMagic) + 8) {
        throw std::runtime_error("trident: store file too small");
    }
    if (std::memcmp(cursor, kMagic, sizeof(kMagic)) != 0) {
        throw std::runtime_error("trident: not a trident store file");
    }
    cursor += sizeof(kMagic);
    std::uint32_t version = read_u32(cursor, end);
    if (version != kFormatVersion) {
        throw std::runtime_error("trident: unsupported store file version");
    }
    StoreCodec codec = static_cast<StoreCodec>(read_u32(cursor, end));
    std::uint64_t dict_count = read_u64(cursor, end);
    (void)read_u64(cursor, end);  // default triple count, recoverable from index
    std::uint64_t named_count = read_u64(cursor, end);

    TripleStore store;
    for (std::uint64_t i = 0; i < dict_count; ++i) {
        store.dict_.intern(read_term(cursor, end));
    }

    const bool map_plain = codec == StoreCodec::Plain;
    auto load_indexes = [&](PermutedIndex& spo, PermutedIndex& pos, PermutedIndex& osp) {
        if (map_plain) {
            load_plain_index(spo, cursor, end, mapping->data(), true);
            load_plain_index(pos, cursor, end, mapping->data(), true);
            load_plain_index(osp, cursor, end, mapping->data(), true);
        } else {
            load_compressed_index(spo, cursor, end);
            load_compressed_index(pos, cursor, end);
            load_compressed_index(osp, cursor, end);
        }
    };

    load_indexes(store.spo_, store.pos_, store.osp_);
    store.staging_.clear();
    store.staging_.reserve(store.spo_.size());
    for (std::size_t i = 0; i < store.spo_.size(); ++i) store.staging_.push_back(store.spo_[i]);

    for (std::uint64_t i = 0; i < named_count; ++i) {
        TermId name(read_u64(cursor, end));
        NamedGraph& graph = store.ensure_named(name);
        load_indexes(graph.spo, graph.pos, graph.osp);
        graph.staging.clear();
        graph.staging.reserve(graph.spo.size());
        for (std::size_t j = 0; j < graph.spo.size(); ++j) {
            graph.staging.push_back(graph.spo[j]);
        }
    }

    if (map_plain) store.mapping_ = std::move(mapping);
    store.built_ = true;
    return store;
}

}  // namespace trident
