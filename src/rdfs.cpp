#include "trident/rdfs.hpp"

#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace trident {

namespace {

struct TripleHash {
    std::size_t operator()(const Triple& t) const noexcept {
        // A cheap mix. The identifiers are dense small integers, so shifting one
        // of them keeps subject and object from cancelling out.
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

// Transitive closure of a small relation, by repeated composition. The schema
// part of an RDF graph is tiny compared with the data, so the simple algorithm
// is the right one here.
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

RdfsStats materialise_rdfs(TripleStore& store) {
    if (!store.built()) store.build();

    RdfsStats stats;
    stats.before = store.triple_count();

    Dictionary& dict = store.dictionary();
    // The vocabulary terms are interned so that they have identifiers even in a
    // graph that never mentions them; the lookups below then need no special case.
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

        // rdfs11 and rdfs5: the closure computed above, written back as triples.
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
            // rdfs7: a triple on a subproperty also holds on the superproperty.
            auto super = subproperty.find(t.p.raw());
            if (super != subproperty.end()) {
                for (TermId p2 : super->second) propose(Triple{t.s, p2, t.o});
            }
            // rdfs2 and rdfs3: domain and range give types to subject and object.
            auto dom = domain_of.find(t.p.raw());
            if (dom != domain_of.end()) {
                for (TermId c : dom->second) propose(Triple{t.s, type, c});
            }
            auto ran = range_of.find(t.p.raw());
            if (ran != range_of.end()) {
                for (TermId c : ran->second) propose(Triple{t.o, type, c});
            }
            // rdfs9: a type is also every superclass of that type.
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
        // Guard against a rule set that fails to reach a fixed point. With this
        // subset it cannot happen, but a silent infinite loop is the worst way to
        // find out otherwise.
        if (stats.rounds > 64) break;
    }

    store.build();
    stats.after = store.triple_count();
    return stats;
}

}  // namespace trident
