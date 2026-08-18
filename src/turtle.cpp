#include "trident/turtle.hpp"

#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace trident {

namespace {

constexpr std::string_view kRdfNs = "http://www.w3.org/1999/02/22-rdf-syntax-ns#";

std::string rdf(std::string_view local) { return std::string(kRdfNs) + std::string(local); }

// Recursive descent over the Turtle grammar. The only state beyond the reader is
// the counter for generated blank nodes and the option set.
class TurtleParser {
public:
    TurtleParser(std::string_view text, const TripleSink& sink, const TurtleOptions& options)
        : reader_(text), sink_(sink), options_(options) {
        reader_.prefixes().base = options.base;
    }

    std::size_t run() {
        for (;;) {
            reader_.skip_ws();
            if (reader_.stream().eof()) break;
            statement();
        }
        return emitted_;
    }

private:
    // --- helpers ----------------------------------------------------------
    CharStream& in() { return reader_.stream(); }

    [[noreturn]] void fail(const std::string& message) { reader_.fail(message); }

    void expect(char c, const char* what) {
        reader_.skip_ws();
        if (in().peek() != c) fail(std::string("expected ") + what);
        in().get();
    }

    void emit(const Term& s, const Term& p, const Term& o) {
        sink_(s, p, o);
        ++emitted_;
    }

    Term fresh_blank() {
        std::string label = options_.blank_scope.empty()
                                ? "genid" + std::to_string(next_blank_++)
                                : options_.blank_scope + "_genid" + std::to_string(next_blank_++);
        return Term::blank(std::move(label));
    }

    Term named_blank(const std::string& label) {
        return Term::blank(options_.blank_scope.empty() ? label
                                                        : options_.blank_scope + "_" + label);
    }

    void require_turtle(const char* construct) {
        if (options_.ntriples_only) {
            fail(std::string(construct) + " is not allowed in N-Triples");
        }
    }

    // Case insensitive comparison for the SPARQL style keywords that Turtle 1.1
    // also accepts (PREFIX and BASE without the leading at sign).
    static bool iequals(std::string_view a, std::string_view b) {
        if (a.size() != b.size()) return false;
        for (std::size_t i = 0; i < a.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(a[i])) !=
                std::tolower(static_cast<unsigned char>(b[i]))) {
                return false;
            }
        }
        return true;
    }

    // --- grammar ----------------------------------------------------------
    void statement() {
        if (in().peek() == '@') {
            require_turtle("a directive");
            directive();
            return;
        }
        if (std::isalpha(in().peek())) {
            // Could be a SPARQL style PREFIX or BASE keyword, or the start of a
            // prefixed name. Look ahead over the word to decide.
            std::size_t save_line = static_cast<std::size_t>(in().line());
            (void)save_line;
            std::string word;
            std::size_t ahead = 0;
            while (reader_.is_pname_char(in().peek(ahead)) && in().peek(ahead) != ':') {
                word += static_cast<char>(in().peek(ahead));
                ++ahead;
            }
            if (in().peek(ahead) != ':' && (iequals(word, "prefix") || iequals(word, "base"))) {
                require_turtle("a directive");
                in().skip(ahead);
                sparql_directive(word);
                return;
            }
        }
        triples();
        reader_.skip_ws();
        if (in().peek() != '.') fail("expected a full stop at the end of a statement");
        in().get();
    }

    void directive() {
        in().get();  // '@'
        std::string name = reader_.read_word();
        reader_.skip_ws();
        if (name == "prefix") {
            std::string prefix;
            while (reader_.is_pname_char(in().peek()) && in().peek() != ':') {
                prefix += static_cast<char>(in().get());
            }
            expect(':', "a colon after the prefix name");
            reader_.skip_ws();
            reader_.prefixes().set(prefix, reader_.read_iriref());
        } else if (name == "base") {
            reader_.prefixes().base = reader_.read_iriref();
        } else {
            fail("unknown directive \"@" + name + "\"");
        }
        expect('.', "a full stop after the directive");
    }

    void sparql_directive(const std::string& keyword) {
        reader_.skip_ws();
        if (iequals(keyword, "base")) {
            reader_.prefixes().base = reader_.read_iriref();
            return;
        }
        std::string prefix;
        while (reader_.is_pname_char(in().peek()) && in().peek() != ':') {
            prefix += static_cast<char>(in().get());
        }
        expect(':', "a colon after the prefix name");
        reader_.skip_ws();
        reader_.prefixes().set(prefix, reader_.read_iriref());
    }

    void triples() {
        reader_.skip_ws();
        Term subject;
        if (in().peek() == '[' && !options_.ntriples_only) {
            // Either an anonymous node with its own properties, or a blank node
            // used purely as a subject placeholder.
            subject = blank_node_property_list();
            reader_.skip_ws();
            if (in().peek() == '.') return;  // the property list was the whole statement
        } else if (in().peek() == '(' && !options_.ntriples_only) {
            subject = collection();
        } else {
            subject = subject_term();
        }
        predicate_object_list(subject);
    }

    Term subject_term() {
        reader_.skip_ws();
        int c = in().peek();
        if (c == '<') return Term::iri(reader_.read_iriref());
        if (c == '_') return named_blank(reader_.read_blank_label());
        require_turtle("a prefixed name");
        return Term::iri(reader_.read_prefixed_name());
    }

    Term predicate_term() {
        reader_.skip_ws();
        int c = in().peek();
        if (c == '<') return Term::iri(reader_.read_iriref());
        if (c == 'a' && !reader_.is_pname_char(in().peek(1))) {
            require_turtle("the keyword a");
            in().get();
            return Term::iri(rdf("type"));
        }
        require_turtle("a prefixed name");
        return Term::iri(reader_.read_prefixed_name());
    }

    Term object_term() {
        reader_.skip_ws();
        int c = in().peek();
        if (c == '<') return Term::iri(reader_.read_iriref());
        if (c == '_') return named_blank(reader_.read_blank_label());
        if (c == '"' || c == '\'') {
            if (c == '\'') require_turtle("a single quoted string");
            if (c == '"' && in().peek(1) == '"' && in().peek(2) == '"') {
                require_turtle("a triple quoted string");
            }
            return reader_.read_literal();
        }
        if (c == '[') {
            require_turtle("a blank node property list");
            return blank_node_property_list();
        }
        if (c == '(') {
            require_turtle("a collection");
            return collection();
        }
        if (is_numeric_start(c) || c == 't' || c == 'f') {
            // true, false and bare numbers are Turtle only. A prefixed name may
            // also start with t or f, so check for the colon first.
            std::size_t ahead = 0;
            while (reader_.is_pname_char(in().peek(ahead))) ++ahead;
            if (in().peek(ahead) != ':') {
                require_turtle("an unquoted literal");
                return reader_.read_numeric_or_boolean();
            }
        }
        require_turtle("a prefixed name");
        return Term::iri(reader_.read_prefixed_name());
    }

    void predicate_object_list(const Term& subject) {
        for (;;) {
            Term predicate = predicate_term();
            object_list(subject, predicate);
            reader_.skip_ws();
            if (in().peek() == ';') {
                require_turtle("a predicate list separated by a semicolon");
                while (in().peek() == ';') {
                    in().get();
                    reader_.skip_ws();
                }
                // A trailing semicolon before the full stop or the closing
                // bracket is allowed.
                if (in().peek() == '.' || in().peek() == ']' || in().eof()) return;
                continue;
            }
            return;
        }
    }

    void object_list(const Term& subject, const Term& predicate) {
        for (;;) {
            Term object = object_term();
            emit(subject, predicate, object);
            reader_.skip_ws();
            if (in().peek() == ',') {
                require_turtle("an object list separated by a comma");
                in().get();
                continue;
            }
            return;
        }
    }

    Term blank_node_property_list() {
        expect('[', "an opening square bracket");
        reader_.skip_ws();
        Term node = fresh_blank();
        if (in().peek() == ']') {
            in().get();
            return node;
        }
        predicate_object_list(node);
        expect(']', "a closing square bracket");
        return node;
    }

    Term collection() {
        expect('(', "an opening parenthesis");
        reader_.skip_ws();
        if (in().peek() == ')') {
            in().get();
            return Term::iri(rdf("nil"));
        }
        Term head = fresh_blank();
        Term current = head;
        for (;;) {
            Term item = object_term();
            emit(current, Term::iri(rdf("first")), item);
            reader_.skip_ws();
            if (in().peek() == ')') {
                in().get();
                emit(current, Term::iri(rdf("rest")), Term::iri(rdf("nil")));
                return head;
            }
            Term next = fresh_blank();
            emit(current, Term::iri(rdf("rest")), next);
            current = next;
        }
    }

    TermReader reader_;
    const TripleSink& sink_;
    TurtleOptions options_;
    std::size_t emitted_ = 0;
    std::size_t next_blank_ = 0;
};

}  // namespace

std::size_t parse_turtle(std::string_view text, const TripleSink& sink,
                         const TurtleOptions& options) {
    TurtleParser parser(text, sink, options);
    return parser.run();
}

std::size_t load_turtle(TripleStore& store, std::string_view text,
                        const TurtleOptions& options) {
    // Triples are staged into a local buffer first, so a syntax error halfway
    // through a document leaves the store untouched rather than half loaded.
    std::vector<Triple> staged;
    Dictionary& dict = store.dictionary();
    std::size_t before = dict.size();
    (void)before;
    std::size_t n = parse_turtle(
        text,
        [&](const Term& s, const Term& p, const Term& o) {
            staged.push_back(Triple{dict.intern(s), dict.intern(p), dict.intern(o)});
        },
        options);
    for (const Triple& t : staged) store.add(t);
    return n;
}

std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("trident: cannot open file " + path);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

std::size_t load_turtle_file(TripleStore& store, const std::string& path,
                             const TurtleOptions& options) {
    std::string text = read_file(path);
    TurtleOptions local = options;
    if (local.blank_scope.empty()) {
        // Derive the scope from the file name so that blank nodes from two files
        // stay apart without the caller having to think about it.
        auto slash = path.find_last_of("/\\");
        std::string stem = slash == std::string::npos ? path : path.substr(slash + 1);
        auto dot = stem.find_last_of('.');
        if (dot != std::string::npos) stem.resize(dot);
        for (char& c : stem) {
            if (!std::isalnum(static_cast<unsigned char>(c))) c = '_';
        }
        local.blank_scope = stem;
    }
    return load_turtle(store, text, local);
}

}  // namespace trident
