#include "trident/dictionary.hpp"

#include <stdexcept>

namespace trident {

TermId Dictionary::intern(const Term& term) {
    std::string key = term.to_ntriples();
    auto it = by_key_.find(key);
    if (it != by_key_.end()) {
        return it->second;
    }
    terms_.push_back(term);
    TermId id = TermId::make(term.kind, terms_.size());  // ordinals start at 1
    by_key_.emplace(std::move(key), id);
    return id;
}

TermId Dictionary::lookup(const Term& term) const {
    auto it = by_key_.find(term.to_ntriples());
    return it == by_key_.end() ? kUnbound : it->second;
}

bool Dictionary::contains(TermId id) const {
    return id.valid() && id.ordinal() >= 1 && id.ordinal() <= terms_.size();
}

const Term& Dictionary::decode(TermId id) const {
    if (!contains(id)) {
        throw std::out_of_range("trident: term identifier not in this dictionary");
    }
    return terms_[static_cast<std::size_t>(id.ordinal() - 1)];
}

std::size_t Dictionary::memory_bytes() const {
    std::size_t bytes = terms_.capacity() * sizeof(Term);
    for (const Term& t : terms_) {
        bytes += t.value.capacity() + t.language.capacity() + t.datatype.capacity();
    }
    // Rough but honest accounting for the open hashing table: one node per entry
    // plus the bucket array.
    for (const auto& entry : by_key_) {
        bytes += entry.first.capacity() + sizeof(void*) * 2 + sizeof(TermId);
    }
    bytes += by_key_.bucket_count() * sizeof(void*);
    return bytes;
}

}  // namespace trident
