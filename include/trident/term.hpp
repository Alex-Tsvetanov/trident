// RDF terms and the encoded identifier that stands in for them everywhere below
// the dictionary. See docs/chapters/03_design.tex, section "Речник на термините".
#pragma once

#include <cstdint>
#include <compare>
#include <ostream>
#include <functional>
#include <string>
#include <string_view>

namespace trident {

enum class TermKind : std::uint8_t {
    Invalid = 0,
    Iri     = 1,
    Blank   = 2,
    Literal = 3,
};

// A term identifier is an opaque 64 bit value: the top 8 bits carry the kind, the
// low 56 bits carry the ordinal handed out by the dictionary. Keeping the kind in
// the identifier lets the execution engine answer isIRI/isLiteral/isBlank without
// touching the reverse dictionary. The wrapper is explicit so that a raw integer
// cannot drift into a position that expects an identifier.
class TermId {
public:
    constexpr TermId() = default;
    constexpr explicit TermId(std::uint64_t raw) : raw_(raw) {}

    static constexpr TermId make(TermKind kind, std::uint64_t ordinal) {
        return TermId((static_cast<std::uint64_t>(kind) << kOrdinalBits) | ordinal);
    }

    constexpr std::uint64_t raw() const { return raw_; }
    constexpr std::uint64_t ordinal() const { return raw_ & kOrdinalMask; }
    constexpr TermKind kind() const {
        return static_cast<TermKind>(raw_ >> kOrdinalBits);
    }
    constexpr bool valid() const { return raw_ != 0; }
    explicit constexpr operator bool() const { return valid(); }

    constexpr auto operator<=>(const TermId&) const = default;

private:
    static constexpr int kOrdinalBits = 56;
    static constexpr std::uint64_t kOrdinalMask = (std::uint64_t{1} << kOrdinalBits) - 1;
    std::uint64_t raw_ = 0;
};

// The identifier that means "no binding". Dictionary ordinals start at one, so
// zero can never collide with a real term.
inline constexpr TermId kUnbound{};

// Datatype IRIs used often enough to be worth naming.
namespace xsd {
inline constexpr std::string_view kString  = "http://www.w3.org/2001/XMLSchema#string";
inline constexpr std::string_view kInteger = "http://www.w3.org/2001/XMLSchema#integer";
inline constexpr std::string_view kDecimal = "http://www.w3.org/2001/XMLSchema#decimal";
inline constexpr std::string_view kDouble  = "http://www.w3.org/2001/XMLSchema#double";
inline constexpr std::string_view kBoolean = "http://www.w3.org/2001/XMLSchema#boolean";
}  // namespace xsd

inline constexpr std::string_view kRdfLangString =
    "http://www.w3.org/1999/02/22-rdf-syntax-ns#langString";

// A decoded RDF term. Literals carry at most one of language or datatype, as
// RDF 1.1 requires: a language tagged literal has datatype rdf:langString.
struct Term {
    TermKind kind = TermKind::Invalid;
    std::string value;      // IRI text, blank node label, or literal lexical form
    std::string language;   // literal only, lower cased
    std::string datatype;   // literal only, IRI text

    static Term iri(std::string v) { return Term{TermKind::Iri, std::move(v), {}, {}}; }
    static Term blank(std::string label) { return Term{TermKind::Blank, std::move(label), {}, {}}; }
    static Term literal(std::string lex) {
        return Term{TermKind::Literal, std::move(lex), {}, std::string(xsd::kString)};
    }
    static Term lang_literal(std::string lex, std::string lang) {
        return Term{TermKind::Literal, std::move(lex), std::move(lang), std::string(kRdfLangString)};
    }
    static Term typed_literal(std::string lex, std::string dt) {
        return Term{TermKind::Literal, std::move(lex), {}, std::move(dt)};
    }

    bool operator==(const Term&) const = default;

    // N-Triples form. This is both the output syntax and the dictionary key, so a
    // term is interned once no matter which syntax it was written in.
    std::string to_ntriples() const;
    // Shorter form for demo output: prefixed names are not resolved, but literals
    // keep their quotes so the term kind stays visible.
    std::string to_display() const;
};

// Escapes a string into the N-Triples literal escape form (without the quotes).
std::string escape_ntriples(std::string_view text);

// Streaming, so that a failed assertion can print what it actually saw.
std::ostream& operator<<(std::ostream& out, const Term& term);
std::ostream& operator<<(std::ostream& out, TermId id);

}  // namespace trident

template <>
struct std::hash<trident::TermId> {
    std::size_t operator()(const trident::TermId& id) const noexcept {
        return std::hash<std::uint64_t>{}(id.raw());
    }
};
