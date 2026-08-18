#include "harness.hpp"

#include <stdexcept>

#include "trident/dictionary.hpp"

using namespace trident;

TEST(dictionary, interning_is_stable_and_reversible) {
    Dictionary dict;
    TermId a = dict.intern(Term::iri("http://example.org/a"));
    TermId again = dict.intern(Term::iri("http://example.org/a"));
    CHECK_EQ(a, again);
    CHECK_EQ(dict.size(), std::size_t{1});
    CHECK_EQ(dict.decode(a).value, std::string("http://example.org/a"));
}

TEST(dictionary, distinct_terms_get_distinct_identifiers) {
    Dictionary dict;
    TermId a = dict.intern(Term::iri("http://example.org/a"));
    TermId b = dict.intern(Term::iri("http://example.org/b"));
    CHECK(a != b);
    CHECK_EQ(dict.size(), std::size_t{2});
}

TEST(dictionary, identifier_carries_the_term_kind) {
    Dictionary dict;
    CHECK(dict.intern(Term::iri("http://example.org/a")).kind() == TermKind::Iri);
    CHECK(dict.intern(Term::blank("b0")).kind() == TermKind::Blank);
    CHECK(dict.intern(Term::literal("text")).kind() == TermKind::Literal);
}

TEST(dictionary, literals_differing_only_in_language_are_different_terms) {
    Dictionary dict;
    TermId en = dict.intern(Term::lang_literal("colour", "en"));
    TermId bg = dict.intern(Term::lang_literal("colour", "bg"));
    TermId plain = dict.intern(Term::literal("colour"));
    CHECK(en != bg);
    CHECK(en != plain);
    CHECK_EQ(dict.size(), std::size_t{3});
}

TEST(dictionary, literals_differing_only_in_datatype_are_different_terms) {
    Dictionary dict;
    TermId integer = dict.intern(Term::typed_literal("1", std::string(xsd::kInteger)));
    TermId decimal = dict.intern(Term::typed_literal("1", std::string(xsd::kDecimal)));
    CHECK(integer != decimal);
}

TEST(dictionary, a_plain_literal_is_an_xsd_string) {
    // RDF 1.1 gives every literal a datatype, and a literal written without one
    // is xsd:string. The two spellings must therefore intern to one term.
    Dictionary dict;
    TermId plain = dict.intern(Term::literal("text"));
    TermId typed = dict.intern(Term::typed_literal("text", std::string(xsd::kString)));
    CHECK_EQ(plain, typed);
    CHECK_EQ(dict.size(), std::size_t{1});
}

TEST(dictionary, lookup_does_not_create_an_entry) {
    Dictionary dict;
    CHECK_EQ(dict.lookup(Term::iri("http://example.org/missing")), kUnbound);
    CHECK_EQ(dict.size(), std::size_t{0});
    dict.intern(Term::iri("http://example.org/present"));
    CHECK(dict.lookup(Term::iri("http://example.org/present")).valid());
}

TEST(dictionary, decoding_an_unknown_identifier_throws) {
    Dictionary dict;
    dict.intern(Term::iri("http://example.org/a"));
    CHECK_THROWS(dict.decode(TermId::make(TermKind::Iri, 99)), std::out_of_range);
    CHECK_THROWS(dict.decode(kUnbound), std::out_of_range);
}

TEST(dictionary, the_unbound_identifier_is_never_handed_out) {
    Dictionary dict;
    for (int i = 0; i < 50; ++i) {
        CHECK(dict.intern(Term::iri("http://example.org/" + std::to_string(i))) != kUnbound);
    }
}

TEST(dictionary, ntriples_form_round_trips_through_the_key) {
    Term literal = Term::typed_literal("a \"quoted\" and \\ escaped\nvalue",
                                       std::string(xsd::kString));
    CHECK_EQ(literal.to_ntriples(),
             std::string("\"a \\\"quoted\\\" and \\\\ escaped\\nvalue\""));
    Dictionary dict;
    TermId id = dict.intern(literal);
    CHECK_EQ(dict.decode(id).value, literal.value);
}
