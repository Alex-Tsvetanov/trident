#include "trident/lexer.hpp"

#include <algorithm>
#include <cctype>
#include <vector>

namespace trident {

std::string ParseError::format(const std::string& message, int line, int column) {
    return "syntax error at line " + std::to_string(line) + ", column " +
           std::to_string(column) + ": " + message;
}

int CharStream::get() {
    if (eof()) return -1;
    int c = static_cast<unsigned char>(text_[pos_++]);
    if (c == '\n') {
        ++line_;
        column_ = 1;
    } else {
        ++column_;
    }
    return c;
}

bool PrefixMap::expand(const std::string& prefix, const std::string& local,
                       std::string& out) const {
    auto it = prefixes.find(prefix);
    if (it == prefixes.end()) return false;
    out = it->second + local;
    return true;
}

namespace {

// Removes "." and ".." segments from a path, per RFC 3986 section 5.2.4.
std::string remove_dot_segments(std::string_view path) {
    std::vector<std::string> out;
    bool trailing_slash = false;
    std::size_t i = 0;
    while (i < path.size()) {
        std::size_t next = path.find('/', i);
        std::string segment(path.substr(i, next == std::string_view::npos ? next : next - i));
        trailing_slash = next != std::string_view::npos;
        if (segment == ".") {
            // drop
        } else if (segment == "..") {
            if (!out.empty()) out.pop_back();
        } else {
            out.push_back(std::move(segment));
        }
        if (next == std::string_view::npos) break;
        i = next + 1;
    }
    std::string result;
    for (std::size_t k = 0; k < out.size(); ++k) {
        if (k) result += '/';
        result += out[k];
    }
    if (trailing_slash) result += '/';
    return result;
}

bool has_scheme(std::string_view ref) {
    for (std::size_t i = 0; i < ref.size(); ++i) {
        char c = ref[i];
        if (c == ':') return i > 0;
        bool ok = std::isalnum(static_cast<unsigned char>(c)) || c == '+' || c == '-' || c == '.';
        if (!ok) return false;
    }
    return false;
}

}  // namespace

std::string resolve_iri(std::string_view base, std::string_view reference) {
    if (reference.empty()) return std::string(base);
    if (has_scheme(reference)) return std::string(reference);
    if (base.empty()) return std::string(reference);

    if (reference.front() == '#') {
        std::string out(base);
        auto hash = out.find('#');
        if (hash != std::string::npos) out.resize(hash);
        return out + std::string(reference);
    }

    // Split the base into scheme plus authority, and path.
    std::size_t scheme_end = base.find("//");
    std::size_t authority_end = base.size();
    if (scheme_end != std::string_view::npos) {
        authority_end = base.find('/', scheme_end + 2);
        if (authority_end == std::string_view::npos) authority_end = base.size();
    } else {
        authority_end = base.find('/');
        if (authority_end == std::string_view::npos) authority_end = base.size();
    }
    std::string root(base.substr(0, authority_end));
    std::string base_path(base.substr(authority_end));
    // Drop any query or fragment from the base path before merging.
    auto cut = base_path.find_first_of("?#");
    if (cut != std::string::npos) base_path.resize(cut);

    if (reference.front() == '/') {
        return root + remove_dot_segments(reference.substr(1)).insert(0, "/");
    }
    if (reference.front() == '?') {
        return root + base_path + std::string(reference);
    }
    auto slash = base_path.find_last_of('/');
    std::string dir = slash == std::string::npos ? "/" : base_path.substr(0, slash + 1);
    std::string merged = dir + std::string(reference);
    bool absolute = !merged.empty() && merged.front() == '/';
    std::string cleaned = remove_dot_segments(absolute ? std::string_view(merged).substr(1)
                                                       : std::string_view(merged));
    return root + (absolute ? "/" : "") + cleaned;
}

bool is_numeric_start(int c) {
    return (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.';
}

bool TermReader::skip_ws() {
    bool any = false;
    for (;;) {
        int c = in_.peek();
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            in_.get();
            any = true;
        } else if (c == '#') {
            while (!in_.eof() && in_.peek() != '\n') in_.get();
            any = true;
        } else {
            return any;
        }
    }
}

bool TermReader::is_pname_start(int c) const {
    if (c < 0) return false;
    // Non ASCII bytes are accepted wholesale: the grammar allows a wide range of
    // Unicode here and the reader works on UTF-8 bytes, so anything above 0x7F is
    // treated as a name character.
    return std::isalpha(static_cast<unsigned char>(c)) || c == '_' || c > 0x7F;
}

bool TermReader::is_pname_char(int c) const {
    if (c < 0) return false;
    return is_pname_start(c) || std::isdigit(static_cast<unsigned char>(c)) || c == '-' ||
           c == '.' || c == '%';
}

std::string TermReader::read_word() {
    std::string out;
    while (is_pname_char(in_.peek())) out += static_cast<char>(in_.get());
    return out;
}

void TermReader::read_escape_into(std::string& out, bool inside_string) {
    in_.get();  // the backslash
    int c = in_.get();
    switch (c) {
        case 't': out += '\t'; return;
        case 'b': out += '\b'; return;
        case 'n': out += '\n'; return;
        case 'r': out += '\r'; return;
        case 'f': out += '\f'; return;
        case '"': out += '"'; return;
        case '\'': out += '\''; return;
        case '\\': out += '\\'; return;
        case 'u':
        case 'U': {
            int digits = (c == 'u') ? 4 : 8;
            unsigned long code = 0;
            for (int i = 0; i < digits; ++i) {
                int d = in_.get();
                if (!std::isxdigit(d)) in_.fail("expected a hexadecimal digit in a unicode escape");
                code = code * 16 + static_cast<unsigned long>(
                    std::isdigit(d) ? d - '0' : (std::tolower(d) - 'a' + 10));
            }
            // Encode as UTF-8 so that the rest of the pipeline can stay byte based.
            if (code < 0x80) {
                out += static_cast<char>(code);
            } else if (code < 0x800) {
                out += static_cast<char>(0xC0 | (code >> 6));
                out += static_cast<char>(0x80 | (code & 0x3F));
            } else if (code < 0x10000) {
                out += static_cast<char>(0xE0 | (code >> 12));
                out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (code & 0x3F));
            } else {
                out += static_cast<char>(0xF0 | (code >> 18));
                out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
                out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (code & 0x3F));
            }
            return;
        }
        default:
            if (!inside_string) in_.fail("unknown escape sequence");
            in_.fail("unknown escape sequence in a string literal");
    }
}

std::string TermReader::read_iriref() {
    if (in_.peek() != '<') in_.fail("expected an IRI in angle brackets");
    in_.get();
    std::string raw;
    for (;;) {
        int c = in_.peek();
        if (c < 0) in_.fail("unterminated IRI");
        if (c == '>') {
            in_.get();
            break;
        }
        if (c == '\\') {
            read_escape_into(raw, false);
            continue;
        }
        if (c == ' ' || c == '\n' || c == '\t' || c == '<' || c == '"' || c == '{' ||
            c == '}' || c == '|' || c == '^' || c == '`') {
            in_.fail("character not allowed inside an IRI");
        }
        raw += static_cast<char>(in_.get());
    }
    return resolve_iri(prefixes_.base, raw);
}

std::string TermReader::read_prefixed_name() {
    std::string prefix;
    while (is_pname_char(in_.peek()) && in_.peek() != ':') {
        prefix += static_cast<char>(in_.get());
    }
    if (in_.peek() != ':') in_.fail("expected a colon in a prefixed name");
    in_.get();
    std::string local;
    while (is_pname_char(in_.peek()) || in_.peek() == ':') {
        // A trailing dot ends the statement rather than the name, unless another
        // name character follows it.
        if (in_.peek() == '.' && !is_pname_char(in_.peek(1))) break;
        local += static_cast<char>(in_.get());
    }
    std::string out;
    if (!prefixes_.expand(prefix, local, out)) {
        in_.fail("undeclared prefix \"" + prefix + "\"");
    }
    return out;
}

std::string TermReader::read_blank_label() {
    if (!(in_.peek() == '_' && in_.peek(1) == ':')) in_.fail("expected a blank node label");
    in_.skip(2);
    std::string label;
    while (is_pname_char(in_.peek())) {
        if (in_.peek() == '.' && !is_pname_char(in_.peek(1))) break;
        label += static_cast<char>(in_.get());
    }
    if (label.empty()) in_.fail("empty blank node label");
    return label;
}

std::string TermReader::read_quoted_string() {
    int quote = in_.peek();
    if (quote != '"' && quote != '\'') in_.fail("expected a quoted string");
    bool triple = in_.peek(1) == quote && in_.peek(2) == quote;
    in_.skip(triple ? 3 : 1);
    std::string out;
    for (;;) {
        int c = in_.peek();
        if (c < 0) in_.fail("unterminated string literal");
        if (c == '\\') {
            read_escape_into(out, true);
            continue;
        }
        if (c == quote) {
            if (!triple) {
                in_.get();
                return out;
            }
            if (in_.peek(1) == quote && in_.peek(2) == quote) {
                in_.skip(3);
                return out;
            }
        }
        if (!triple && (c == '\n' || c == '\r')) {
            in_.fail("newline inside a single quoted string literal");
        }
        out += static_cast<char>(in_.get());
    }
}

Term TermReader::read_literal() {
    std::string lexical = read_quoted_string();
    if (in_.peek() == '@') {
        in_.get();
        std::string tag;
        while (std::isalnum(in_.peek()) || in_.peek() == '-') {
            tag += static_cast<char>(std::tolower(in_.get()));
        }
        if (tag.empty()) in_.fail("empty language tag");
        return Term::lang_literal(std::move(lexical), std::move(tag));
    }
    if (in_.peek() == '^' && in_.peek(1) == '^') {
        in_.skip(2);
        std::string datatype = in_.peek() == '<' ? read_iriref() : read_prefixed_name();
        return Term::typed_literal(std::move(lexical), std::move(datatype));
    }
    return Term::literal(std::move(lexical));
}

Term TermReader::read_numeric_or_boolean() {
    if (in_.peek() == 't' || in_.peek() == 'f') {
        std::string word = read_word();
        if (word == "true" || word == "false") {
            return Term::typed_literal(word, std::string(xsd::kBoolean));
        }
        in_.fail("expected true or false");
    }
    std::string text;
    bool decimal = false, exponent = false;
    if (in_.peek() == '+' || in_.peek() == '-') text += static_cast<char>(in_.get());
    while (std::isdigit(in_.peek())) text += static_cast<char>(in_.get());
    if (in_.peek() == '.' && std::isdigit(in_.peek(1))) {
        decimal = true;
        text += static_cast<char>(in_.get());
        while (std::isdigit(in_.peek())) text += static_cast<char>(in_.get());
    }
    if (in_.peek() == 'e' || in_.peek() == 'E') {
        exponent = true;
        text += static_cast<char>(in_.get());
        if (in_.peek() == '+' || in_.peek() == '-') text += static_cast<char>(in_.get());
        if (!std::isdigit(in_.peek())) in_.fail("expected digits in the exponent");
        while (std::isdigit(in_.peek())) text += static_cast<char>(in_.get());
    }
    if (text.empty() || text == "+" || text == "-") in_.fail("expected a number");
    std::string_view datatype = exponent ? xsd::kDouble : (decimal ? xsd::kDecimal : xsd::kInteger);
    return Term::typed_literal(std::move(text), std::string(datatype));
}

}  // namespace trident
