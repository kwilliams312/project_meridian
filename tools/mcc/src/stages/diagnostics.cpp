// tools/mcc/src/stages/diagnostics.cpp — text/JSON rendering of diagnostics.

#include "stages/diagnostics.h"

#include <algorithm>

namespace mcc::diag {

namespace {

// Minimal JSON string escaping (RFC 8259): quotes, backslash, control chars.
std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    static const char* hex = "0123456789abcdef";
                    out += "\\u00";
                    out += hex[(c >> 4) & 0xF];
                    out += hex[c & 0xF];
                } else {
                    out += c;
                }
        }
    }
    return out;
}

const char* severity_str(Severity s) {
    return s == Severity::Error ? "error" : "warning";
}

}  // namespace

std::string format_text(const Diagnostic& d) {
    // Mirrors validate_content.py's message form: "<RULE> <file>[ at <where>]: <message>".
    std::string line = d.rule + " " + d.file;
    if (!d.where.empty()) line += " at " + d.where;
    line += ": " + d.message;
    return line;
}

void render_text(const Diagnostics& diags, const std::string& stats_line, std::ostream& out) {
    out << stats_line << '\n';

    std::vector<const Diagnostic*> warnings;
    std::vector<const Diagnostic*> errors;
    for (const auto& d : diags.items()) {
        (d.severity == Severity::Warning ? warnings : errors).push_back(&d);
    }

    if (!warnings.empty()) {
        out << '\n' << warnings.size() << " warning(s):\n";
        for (const auto* d : warnings) out << "  " << format_text(*d) << '\n';
    }
    if (!errors.empty()) {
        out << '\n' << errors.size() << " error(s):\n";
        for (const auto* d : errors) out << "  " << format_text(*d) << '\n';
    } else {
        out << "OK — structural lints pass, all content references resolve.\n";
    }
}

void render_json(const Diagnostics& diags, std::ostream& out) {
    const std::size_t errs = diags.error_count();
    out << "{\n";
    out << "  \"ok\": " << (errs == 0 ? "true" : "false") << ",\n";
    out << "  \"error_count\": " << errs << ",\n";
    out << "  \"warning_count\": " << diags.warning_count() << ",\n";
    out << "  \"diagnostics\": [";
    const auto& items = diags.items();
    for (std::size_t i = 0; i < items.size(); ++i) {
        const auto& d = items[i];
        out << (i == 0 ? "\n" : ",\n");
        out << "    {\"rule\": \"" << json_escape(d.rule) << "\", "
            << "\"severity\": \"" << severity_str(d.severity) << "\", "
            << "\"file\": \"" << json_escape(d.file) << "\", "
            << "\"where\": \"" << json_escape(d.where) << "\", "
            << "\"message\": \"" << json_escape(d.message) << "\"}";
    }
    if (!items.empty()) out << "\n  ";
    out << "]\n";
    out << "}\n";
}

}  // namespace mcc::diag
