#include "trident/store.hpp"

#include <algorithm>
#include <stdexcept>

namespace trident {

const char* index_name(IndexOrder order) {
    switch (order) {
        case IndexOrder::Spo: return "SPO";
        case IndexOrder::Pos: return "POS";
        case IndexOrder::Osp: return "OSP";
    }
    return "???";
}

namespace {

std::array<int, 3> permutation_of(IndexOrder order) {
    switch (order) {
        case IndexOrder::Spo: return {0, 1, 2};
        case IndexOrder::Pos: return {1, 2, 0};
        case IndexOrder::Osp: return {2, 0, 1};
    }
    return {0, 1, 2};
}

}  // namespace

PermutedIndex::PermutedIndex(IndexOrder order)
    : order_(order), perm_(permutation_of(order)) {}

void PermutedIndex::assign(std::vector<Triple> triples) {
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

PermutedIndex::Range PermutedIndex::prefix_range(const Triple& key, int prefix_len) const {
    if (prefix_len <= 0) {
        return Range{0, data_.size()};
    }
    const std::array<int, 3> perm = perm_;
    const int n = prefix_len;
    auto less = [perm, n](const Triple& a, const Triple& b) {
        for (int i = 0; i < n; ++i) {
            if (a[perm[i]] != b[perm[i]]) return a[perm[i]] < b[perm[i]];
        }
        return false;
    };
    auto lo = std::lower_bound(data_.begin(), data_.end(), key, less);
    auto hi = std::upper_bound(lo, data_.end(), key, less);
    return Range{static_cast<std::size_t>(lo - data_.begin()),
                 static_cast<std::size_t>(hi - data_.begin())};
}

IndexChoice choose_index(const EncodedPattern& pattern) {
    // For each index, count the leading components under its permutation that are
    // bound. That count is the length of the sorted prefix a scan can use.
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

void TripleStore::add(const Triple& t) {
    staging_.push_back(t);
    built_ = false;
}

void TripleStore::add(const Term& s, const Term& p, const Term& o) {
    add(Triple{dict_.intern(s), dict_.intern(p), dict_.intern(o)});
}

void TripleStore::build() {
    spo_.assign(staging_);
    pos_.assign(staging_);
    osp_.assign(staging_);
    // Keep the staged copy in the deduplicated order the indexes settled on, so a
    // later add() followed by build() merges rather than losing the earlier data.
    staging_.clear();
    staging_.reserve(spo_.size());
    for (std::size_t i = 0; i < spo_.size(); ++i) staging_.push_back(spo_[i]);
    built_ = true;
}

std::size_t TripleStore::triple_count() const { return spo_.size(); }

const PermutedIndex& TripleStore::index(IndexOrder order) const {
    switch (order) {
        case IndexOrder::Spo: return spo_;
        case IndexOrder::Pos: return pos_;
        case IndexOrder::Osp: return osp_;
    }
    return spo_;
}

std::size_t TripleStore::count(const EncodedPattern& pattern) const {
    if (!built_) throw std::logic_error("trident: TripleStore::build() has not been called");
    IndexChoice choice = choose_index(pattern);
    Triple key{pattern.s, pattern.p, pattern.o};
    return index(choice.order).prefix_range(key, choice.prefix_len).count();
}

void TripleStore::write_ntriples(std::ostream& out) const {
    for (std::size_t i = 0; i < spo_.size(); ++i) {
        const Triple& t = spo_[i];
        out << dict_.decode(t.s).to_ntriples() << ' '
            << dict_.decode(t.p).to_ntriples() << ' '
            << dict_.decode(t.o).to_ntriples() << " .\n";
    }
}

std::size_t TripleStore::memory_bytes() const {
    return dict_.memory_bytes() + spo_.memory_bytes() + pos_.memory_bytes() +
           osp_.memory_bytes() + staging_.capacity() * sizeof(Triple);
}

}  // namespace trident
