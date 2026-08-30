#include "trident/rdfs.hpp"

#include <unordered_set>

namespace trident {

namespace {

struct TripleHash {
    std::size_t operator()(const Triple& t) const noexcept {
        std::size_t h = static_cast<std::size_t>(t.s.raw());
        h = h * 1000003u + static_cast<std::size_t>(t.p.raw());
        h = h * 1000003u + static_cast<std::size_t>(t.o.raw());
        return h;
    }
};

using TripleSet = std::unordered_set<Triple, TripleHash>;
using Relation = std::unordered_map<std::uint64_t, std::vector<TermId>>;

void insert_unique(std::vector<TermId>& list, TermId value) {
    for (TermId existing : list) {
        if (existing == value) return;
    }
    list.push_back(value);
}

void close_transitively(Relation& relation) {
    bool changed = true;
    while (changed) {
        changed = false;
        for (auto& entry : relation) {
            std::vector<TermId> additions;
            for (TermId middle : entry.second) {
                auto it = relation.find(middle.raw());
                if (it == relation.end()) continue;
                for (TermId target : it->second) {
                    bool present = false;
                    for (TermId existing : entry.second) {
                        if (existing == target) {
                            present = true;
                            break;
                        }
                    }
                    if (!present) additions.push_back(target);
                }
            }
            for (TermId target : additions) {
                insert_unique(entry.second, target);
                changed = true;
            }
        }
    }
}

}  // namespace

std::vector<TermId> RdfsSchema::lookup(const Relation& relation, TermId key) const {
    auto it = relation.find(key.raw());
    if (it == relation.end()) return {};
    return it->second;
}

RdfsSchema::RdfsSchema(TripleStore& store) {
    Dictionary& dict = store.dictionary();
    TermId sub_class = dict.intern(Term::iri(std::string(rdfs_iri::kSubClassOf)));
    TermId sub_property = dict.intern(Term::iri(std::string(rdfs_iri::kSubPropertyOf)));
    TermId domain = dict.intern(Term::iri(std::string(rdfs_iri::kDomain)));
    TermId range = dict.intern(Term::iri(std::string(rdfs_iri::kRange)));
    type_ = dict.intern(Term::iri(std::string(rdfs_iri::kType)));

    const PermutedIndex& spo = store.index(IndexOrder::Spo);
    for (std::size_t i = 0; i < spo.size(); ++i) {
        const Triple& t = spo[i];
        if (sub_class.valid() && t.p == sub_class) insert_unique(super_class_[t.s.raw()], t.o);
        else if (sub_property.valid() && t.p == sub_property) {
            insert_unique(super_property_[t.s.raw()], t.o);
        } else if (domain.valid() && t.p == domain) {
            insert_unique(domain_of_[t.s.raw()], t.o);
        } else if (range.valid() && t.p == range) {
            insert_unique(range_of_[t.s.raw()], t.o);
        }
    }
    close_transitively(super_class_);
    close_transitively(super_property_);

    for (const auto& entry : super_class_) {
        TermId child(entry.first);
        for (TermId parent : entry.second) insert_unique(sub_class_[parent.raw()], child);
    }
    for (const auto& entry : super_property_) {
        TermId child(entry.first);
        for (TermId parent : entry.second) insert_unique(sub_property_[parent.raw()], child);
    }
}

std::vector<TermId> RdfsSchema::properties_entailing(TermId property) const {
    std::vector<TermId> out = lookup(sub_property_, property);
    insert_unique(out, property);
    return out;
}

std::vector<TermId> RdfsSchema::classes_entailing(TermId klass) const {
    std::vector<TermId> out = lookup(sub_class_, klass);
    insert_unique(out, klass);
    return out;
}

std::vector<TermId> RdfsSchema::properties_with_domain(TermId klass) const {
    std::vector<TermId> classes = classes_entailing(klass);
    std::vector<TermId> out;
    for (const auto& entry : domain_of_) {
        for (TermId declared : entry.second) {
            for (TermId c : classes) {
                if (declared == c) {
                    insert_unique(out, TermId(entry.first));
                    break;
                }
            }
        }
    }
    return out;
}

std::vector<TermId> RdfsSchema::properties_with_range(TermId klass) const {
    std::vector<TermId> classes = classes_entailing(klass);
    std::vector<TermId> out;
    for (const auto& entry : range_of_) {
        for (TermId declared : entry.second) {
            for (TermId c : classes) {
                if (declared == c) {
                    insert_unique(out, TermId(entry.first));
                    break;
                }
            }
        }
    }
    return out;
}

RdfsStats materialise_rdfs(TripleStore& store) {
    if (!store.built()) store.build();

    RdfsStats stats;
    stats.before = store.triple_count();

    Dictionary& dict = store.dictionary();
    const TermId sub_class = dict.intern(Term::iri(std::string(rdfs_iri::kSubClassOf)));
    const TermId sub_property = dict.intern(Term::iri(std::string(rdfs_iri::kSubPropertyOf)));
    const TermId domain = dict.intern(Term::iri(std::string(rdfs_iri::kDomain)));
    const TermId range = dict.intern(Term::iri(std::string(rdfs_iri::kRange)));
    const TermId type = dict.intern(Term::iri(std::string(rdfs_iri::kType)));

    const PermutedIndex& spo = store.index(IndexOrder::Spo);
    std::vector<Triple> all;
    all.reserve(spo.size());
    TripleSet known;
    known.reserve(spo.size() * 2);
    for (std::size_t i = 0; i < spo.size(); ++i) {
        all.push_back(spo[i]);
        known.insert(spo[i]);
    }

    for (;;) {
        ++stats.rounds;
        Relation subclass, subproperty, domain_of, range_of;
        for (const Triple& t : all) {
            if (t.p == sub_class) insert_unique(subclass[t.s.raw()], t.o);
            else if (t.p == sub_property) insert_unique(subproperty[t.s.raw()], t.o);
            else if (t.p == domain) insert_unique(domain_of[t.s.raw()], t.o);
            else if (t.p == range) insert_unique(range_of[t.s.raw()], t.o);
        }
        close_transitively(subclass);
        close_transitively(subproperty);

        std::vector<Triple> fresh;
        auto propose = [&](const Triple& candidate) {
            if (known.insert(candidate).second) fresh.push_back(candidate);
        };

        for (const auto& entry : subclass) {
            for (TermId target : entry.second) {
                propose(Triple{TermId(entry.first), sub_class, target});
            }
        }
        for (const auto& entry : subproperty) {
            for (TermId target : entry.second) {
                propose(Triple{TermId(entry.first), sub_property, target});
            }
        }

        const std::size_t scanned = all.size();
        for (std::size_t i = 0; i < scanned; ++i) {
            const Triple t = all[i];
            auto super = subproperty.find(t.p.raw());
            if (super != subproperty.end()) {
                for (TermId p2 : super->second) propose(Triple{t.s, p2, t.o});
            }
            auto dom = domain_of.find(t.p.raw());
            if (dom != domain_of.end()) {
                for (TermId c : dom->second) propose(Triple{t.s, type, c});
            }
            auto ran = range_of.find(t.p.raw());
            if (ran != range_of.end()) {
                for (TermId c : ran->second) propose(Triple{t.o, type, c});
            }
            if (t.p == type) {
                auto up = subclass.find(t.o.raw());
                if (up != subclass.end()) {
                    for (TermId c : up->second) propose(Triple{t.s, type, c});
                }
            }
        }

        if (fresh.empty()) break;
        all.insert(all.end(), fresh.begin(), fresh.end());
        stats.inferred += fresh.size();
        for (const Triple& t : fresh) store.add(t);
        if (stats.rounds > 64) break;
    }

    store.build();
    stats.after = store.triple_count();
    return stats;
}

}  // namespace trident
