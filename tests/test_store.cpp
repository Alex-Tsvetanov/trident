#include "harness.hpp"

#include <stdexcept>
#include <string>

#include "trident/store.hpp"
#include "trident/turtle.hpp"

using namespace trident;

namespace {

TripleStore load_small() {
    TripleStore store;
    load_turtle_file(store, std::string(TRIDENT_DATA_DIR) + "/small.ttl");
    store.build();
    return store;
}

TermId id_of(const TripleStore& store, const std::string& iri) {
    return store.encode(Term::iri("http://example.org/" + iri));
}

}  // namespace

TEST(store, the_small_graph_has_forty_triples) {
    TripleStore store = load_small();
    CHECK_EQ(store.triple_count(), std::size_t{40});
}

TEST(store, all_three_indexes_hold_the_same_set) {
    TripleStore store = load_small();
    CHECK_EQ(store.index(IndexOrder::Spo).size(), std::size_t{40});
    CHECK_EQ(store.index(IndexOrder::Pos).size(), std::size_t{40});
    CHECK_EQ(store.index(IndexOrder::Osp).size(), std::size_t{40});
}

TEST(store, every_index_is_sorted_in_its_own_permutation) {
    TripleStore store = load_small();
    for (IndexOrder order : {IndexOrder::Spo, IndexOrder::Pos, IndexOrder::Osp}) {
        const PermutedIndex& index = store.index(order);
        const auto& perm = index.permutation();
        for (std::size_t i = 1; i < index.size(); ++i) {
            const Triple& previous = index[i - 1];
            const Triple& current = index[i];
            bool ordered = false;
            for (int c = 0; c < 3; ++c) {
                if (previous[perm[c]] != current[perm[c]]) {
                    ordered = previous[perm[c]] < current[perm[c]];
                    break;
                }
            }
            CHECK(ordered);
        }
    }
}

TEST(store, choose_index_covers_every_binding_combination) {
    // The claim the whole three index design rests on: for any of the eight
    // combinations of bound positions, one of the three permutations has all the
    // bound positions as a prefix.
    TermId bound{7};
    for (int mask = 0; mask < 8; ++mask) {
        EncodedPattern pattern;
        int expected = 0;
        if (mask & 1) { pattern.s = bound; ++expected; }
        if (mask & 2) { pattern.p = bound; ++expected; }
        if (mask & 4) { pattern.o = bound; ++expected; }
        IndexChoice choice = choose_index(pattern);
        CHECK_EQ(choice.prefix_len, expected);
    }
}

TEST(store, counts_match_hand_counted_answers) {
    TripleStore store = load_small();
    EncodedPattern any;
    CHECK_EQ(store.count(any), std::size_t{40});

    EncodedPattern by_predicate;
    by_predicate.p = id_of(store, "name");
    CHECK_EQ(store.count(by_predicate), std::size_t{4});  // alice, bob, carol, dave

    EncodedPattern by_subject;
    by_subject.s = id_of(store, "alice");
    CHECK_EQ(store.count(by_subject), std::size_t{6});

    EncodedPattern by_object;
    by_object.o = id_of(store, "carol");
    // alice knows carol, bob knows carol, book4 has carol as author
    CHECK_EQ(store.count(by_object), std::size_t{3});

    EncodedPattern subject_and_predicate;
    subject_and_predicate.s = id_of(store, "alice");
    subject_and_predicate.p = id_of(store, "knows");
    CHECK_EQ(store.count(subject_and_predicate), std::size_t{2});

    EncodedPattern exact;
    exact.s = id_of(store, "alice");
    exact.p = id_of(store, "knows");
    exact.o = id_of(store, "bob");
    CHECK_EQ(store.count(exact), std::size_t{1});
}

TEST(store, an_unknown_constant_matches_nothing) {
    TripleStore store = load_small();
    CHECK_EQ(store.encode(Term::iri("http://example.org/nobody")), kUnbound);
}

TEST(store, duplicates_are_removed_by_the_index_build) {
    TripleStore store;
    Term s = Term::iri("http://a/s"), p = Term::iri("http://a/p"), o = Term::iri("http://a/o");
    store.add(s, p, o);
    store.add(s, p, o);
    store.add(s, p, o);
    CHECK_EQ(store.staged_count(), std::size_t{3});
    store.build();
    CHECK_EQ(store.triple_count(), std::size_t{1});
}

TEST(store, a_second_build_keeps_the_earlier_triples) {
    TripleStore store;
    store.add(Term::iri("http://a/s1"), Term::iri("http://a/p"), Term::iri("http://a/o"));
    store.build();
    store.add(Term::iri("http://a/s2"), Term::iri("http://a/p"), Term::iri("http://a/o"));
    store.build();
    CHECK_EQ(store.triple_count(), std::size_t{2});
}

TEST(store, scanning_before_build_is_refused) {
    TripleStore store;
    store.add(Term::iri("http://a/s"), Term::iri("http://a/p"), Term::iri("http://a/o"));
    CHECK(!store.built());
    CHECK_THROWS(store.count(EncodedPattern{}), std::logic_error);
}

TEST(store, ntriples_file_loads_with_language_tag_and_blank_node) {
    TripleStore store;
    TurtleOptions strict;
    strict.ntriples_only = true;
    std::size_t n =
        load_turtle_file(store, std::string(TRIDENT_DATA_DIR) + "/small.nt", strict);
    store.build();
    CHECK_EQ(n, std::size_t{8});
    CHECK_EQ(store.triple_count(), std::size_t{8});
    // The blank node label was rewritten with the document scope.
    CHECK(store.encode(Term::blank("small_shared")).valid());
    CHECK(store.encode(Term::lang_literal("\xd0\x90\xd0\xbb\xd0\xb5\xd0\xba\xd1\x81", "bg"))
              .valid());
}

TEST(store, ntriples_output_round_trips_through_the_parser) {
    TripleStore first = load_small();
    std::ostringstream written;
    first.write_ntriples(written);

    TripleStore second;
    TurtleOptions strict;
    strict.ntriples_only = true;
    load_turtle(second, written.str(), strict);
    second.build();
    CHECK_EQ(second.triple_count(), first.triple_count());
}
