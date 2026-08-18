// The character reader shared by the Turtle parser and the SPARQL parser, plus
// the term level lexing both of them need. Sharing this layer is why a syntax
// error reads the same whichever grammar produced it.
#pragma once

#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>

#include "trident/term.hpp"

namespace trident {

class ParseError : public std::runtime_error {
public:
    ParseError(std::string message, int line, int column)
        : std::runtime_error(format(message, line, column)),
          line_(line), column_(column), message_(std::move(message)) {}

    int line() const { return line_; }
    int column() const { return column_; }
    const std::string& message() const { return message_; }

private:
    static std::string format(const std::string& message, int line, int column);
    int line_, column_;
    std::string message_;
};

// A forward only reader over a text buffer that keeps line and column, and
// offers one character of lookahead plus a cheap multi character peek for the
// few places where one is not enough (triple quoted strings, "^^", "||").
class CharStream {
public:
    explicit CharStream(std::string_view text) : text_(text) {}

    bool eof() const { return pos_ >= text_.size(); }
    // Returns -1 past the end so that callers can compare against a character
    // without a separate eof() test.
    int peek(std::size_t ahead = 0) const {
        std::size_t at = pos_ + ahead;
        return at < text_.size() ? static_cast<unsigned char>(text_[at]) : -1;
    }
    bool looking_at(std::string_view what) const {
        return text_.compare(pos_, what.size(), what) == 0;
    }
    int get();
    void skip(std::size_t n) {
        for (std::size_t i = 0; i < n && !eof(); ++i) get();
    }

    int line() const { return line_; }
    int column() const { return column_; }
    std::size_t position() const { return pos_; }

    [[noreturn]] void fail(const std::string& message) const {
        throw ParseError(message, line_, column_);
    }

private:
    std::string_view text_;
    std::size_t pos_ = 0;
    int line_ = 1;
    int column_ = 1;
};

// Prefix declarations, plus the base IRI against which relative references are
// resolved. Both grammars carry exactly this state.
struct PrefixMap {
    std::string base;
    std::unordered_map<std::string, std::string> prefixes;

    void set(std::string prefix, std::string iri) {
        prefixes[std::move(prefix)] = std::move(iri);
    }
    // Returns false when the prefix was never declared.
    bool expand(const std::string& prefix, const std::string& local, std::string& out) const;
};

// Resolution of a relative reference against a base IRI. This covers the forms
// that occur in RDF documents: empty reference, fragment, absolute path, and
// relative path, with dot segments removed. It is not a complete RFC 3986
// implementation and does not try to be; see docs, section on the syntax layer.
std::string resolve_iri(std::string_view base, std::string_view reference);

// Term level lexing, shared by both grammars.
class TermReader {
public:
    explicit TermReader(std::string_view text) : in_(text) {}

    CharStream& stream() { return in_; }
    const CharStream& stream() const { return in_; }
    PrefixMap& prefixes() { return prefixes_; }
    const PrefixMap& prefixes() const { return prefixes_; }

    // Whitespace and comments. Returns true when anything was consumed.
    bool skip_ws();

    bool is_pname_start(int c) const;
    bool is_pname_char(int c) const;

    // <...>, resolved against the base IRI when relative.
    std::string read_iriref();
    // prefix:local or :local. Throws when the prefix was not declared.
    std::string read_prefixed_name();
    // _:label, returning the bare label.
    std::string read_blank_label();
    // A quoted string in any of the four Turtle forms, with escapes expanded.
    std::string read_quoted_string();
    // A bare word made of name characters, used for keywords such as PREFIX.
    std::string read_word();

    // Reads a complete literal starting at the opening quote, including any
    // language tag or datatype suffix.
    Term read_literal();
    // Reads a numeric or boolean literal written without quotes.
    Term read_numeric_or_boolean();

    [[noreturn]] void fail(const std::string& message) const { in_.fail(message); }

private:
    void read_escape_into(std::string& out, bool inside_string);
    CharStream in_;
    PrefixMap prefixes_;
};

// True for the characters that may start a numeric literal.
bool is_numeric_start(int c);

}  // namespace trident
