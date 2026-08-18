#include "trident/term.hpp"

namespace trident {

std::string escape_ntriples(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += c;      break;
        }
    }
    return out;
}

std::string Term::to_ntriples() const {
    switch (kind) {
        case TermKind::Iri:
            return "<" + value + ">";
        case TermKind::Blank:
            return "_:" + value;
        case TermKind::Literal: {
            std::string out = "\"" + escape_ntriples(value) + "\"";
            if (!language.empty()) {
                out += "@" + language;
            } else if (!datatype.empty() && datatype != xsd::kString) {
                out += "^^<" + datatype + ">";
            }
            return out;
        }
        case TermKind::Invalid:
        default:
            return "<INVALID>";
    }
}

std::string Term::to_display() const {
    if (kind == TermKind::Iri) {
        // Show the local part when the IRI has an obvious separator. Full IRIs
        // make the demo output unreadable at eighty columns.
        auto cut = value.find_last_of("#/");
        if (cut != std::string::npos && cut + 1 < value.size()) {
            return ":" + value.substr(cut + 1);
        }
        return "<" + value + ">";
    }
    if (kind == TermKind::Literal && language.empty() && !datatype.empty() &&
        datatype != xsd::kString) {
        constexpr std::string_view kXsd = "http://www.w3.org/2001/XMLSchema#";
        if (datatype.compare(0, kXsd.size(), kXsd) == 0) {
            return "\"" + escape_ntriples(value) + "\"^^xsd:" + datatype.substr(kXsd.size());
        }
    }
    return to_ntriples();
}

std::ostream& operator<<(std::ostream& out, const Term& term) {
    return out << term.to_ntriples();
}

std::ostream& operator<<(std::ostream& out, TermId id) {
    return out << "id(" << static_cast<int>(id.kind()) << ":" << id.ordinal() << ")";
}

}  // namespace trident
