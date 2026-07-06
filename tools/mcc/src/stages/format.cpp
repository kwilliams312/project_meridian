// tools/mcc/src/stages/format.cpp — canonical YAML emitter (`mcc fmt`).
//
// Design: we do NOT use yaml-cpp's default Emitter (it drops comments, folds
// block scalars, and re-plain-izes quoted strings — all semantic or authoring
// losses). Instead we walk the parsed node tree ourselves and emit each node
// under explicit canonical rules (see tools/mcc/FORMAT.md), keying scalar
// quoting off the source Tag() so resolved types never change. Comments are
// lifted from the raw source by line and re-attached to the node they annotate.

#include "stages/format.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <yaml-cpp/yaml.h>

namespace fs = std::filesystem;

namespace mcc::stages {

namespace {

constexpr int kIndentStep = 2;  // canonical indent: two spaces per level.

// ---- Comment capture -------------------------------------------------------
//
// yaml-cpp discards comments, so we scan the raw source separately. For each
// source line we record: any run of full-line comments that immediately
// precede it (its "leading" block) and a trailing "# ..." on the line itself.
// Nodes carry a source Mark() (1-based line); we look comments up by that line.

struct LineComments {
    std::vector<std::string> leading;  // full-line comments directly above
    std::string trailing;              // end-of-line comment on this line (incl. '#')
};

// Return the byte offset of a real (non-quoted, non-block-scalar) '#' that
// starts a trailing comment on `line`, or npos if none. We intentionally use a
// conservative heuristic: a '#' preceded by whitespace and not inside a quote.
std::size_t find_trailing_hash(const std::string& line) {
    bool in_single = false, in_double = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (c == '\'' && !in_double) in_single = !in_single;
        else if (c == '"' && !in_single) in_double = !in_double;
        else if (c == '#' && !in_single && !in_double) {
            // A comment '#' must be at line start or follow whitespace.
            if (i == 0 || std::isspace(static_cast<unsigned char>(line[i - 1]))) return i;
        }
    }
    return std::string::npos;
}

std::string rstrip(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    return s;
}
std::string lstrip(std::string s) {
    std::size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    return s.substr(i);
}

// Map from 1-based source line -> its captured comments. Only lines that carry
// a comment (leading or trailing) appear. Leading comments attach to the line
// of the *next* content, matching how a reader groups them with what follows.
class CommentIndex {
public:
    explicit CommentIndex(const std::string& src) { build(src); }

    // Leading comment block for the node whose value begins on `line`, consumed
    // once (so a comment is never emitted twice). Returns full-line comment text
    // without the trailing newline, already '#'-prefixed and left-trimmed.
    std::vector<std::string> take_leading(int line) {
        auto it = by_line_.find(line);
        if (it == by_line_.end()) return {};
        std::vector<std::string> out = std::move(it->second.leading);
        it->second.leading.clear();
        return out;
    }
    // Trailing comment for `line` (incl. the leading '#'), or "" if none.
    std::string trailing(int line) const {
        auto it = by_line_.find(line);
        return it == by_line_.end() ? std::string() : it->second.trailing;
    }
    // Any file-trailing comments that followed the last content line.
    const std::vector<std::string>& footer() const { return footer_; }

private:
    void build(const std::string& src) {
        std::vector<std::string> lines;
        std::stringstream ss(src);
        std::string l;
        while (std::getline(ss, l)) {
            if (!l.empty() && l.back() == '\r') l.pop_back();
            lines.push_back(l);
        }
        std::vector<std::string> pending;  // leading comments awaiting content
        for (std::size_t i = 0; i < lines.size(); ++i) {
            const std::string& line = lines[i];
            const std::string trimmed = lstrip(line);
            if (trimmed.empty()) {
                // Blank line: flush pending leading comments as footer-ish; a
                // blank separates a comment block from later content, so we drop
                // the association (canonical form omits blank lines anyway).
                pending.clear();
                continue;
            }
            if (trimmed[0] == '#') {
                pending.push_back(rstrip(trimmed));
                continue;
            }
            // Content line at 1-based (i+1). Attach any pending leading block.
            const int lineno = static_cast<int>(i + 1);
            LineComments& lc = by_line_[lineno];
            if (!pending.empty()) {
                lc.leading = std::move(pending);
                pending.clear();
            }
            const std::size_t h = find_trailing_hash(line);
            if (h != std::string::npos) lc.trailing = rstrip(line.substr(h));
        }
        // Comments after the final content line become the file footer.
        footer_ = std::move(pending);
    }

    std::map<int, LineComments> by_line_;
    std::vector<std::string> footer_;
};

// ---- Scalar canonicalization ----------------------------------------------

// True if `s` would, as a bare plain scalar, resolve to a non-string YAML type
// (null/bool/int/float) or is otherwise unsafe to emit unquoted. Such scalars
// must be quoted when they are semantically strings, and — conversely — a plain
// scalar with tag '?' that matches these keeps its bare form (its type IS that
// non-string). We use this only to decide quoting for STRING-typed scalars.
bool looks_like_non_string(const std::string& s) {
    if (s.empty()) return true;  // empty plain scalar resolves to null
    // YAML 1.1/1.2 core + common bool-ish tokens yaml-cpp resolves.
    static const char* kSpecials[] = {
        "null", "Null", "NULL", "~",
        "true", "True", "TRUE", "false", "False", "FALSE",
        "yes", "Yes", "YES", "no", "No", "NO",
        "on", "On", "ON", "off", "Off", "OFF"};
    for (const char* k : kSpecials)
        if (s == k) return true;
    // Numeric (int / float / hex / sci). Cheap check: try to fully parse.
    char* end = nullptr;
    std::strtod(s.c_str(), &end);
    if (end == s.c_str() + s.size()) return true;  // whole string is a number
    // Leading zero like "01" resolves oddly; treat as needing quotes.
    return false;
}

// Does a scalar need double-quoting for SAFETY (chars that break plain flow /
// indicator-leading)? Kept minimal — most content scalars are plain-safe.
bool needs_quote_for_safety(const std::string& s) {
    if (s.empty()) return true;
    // Leading indicators that YAML forbids at the start of a plain scalar.
    static const std::string kBadLead = "!&*?|>%@`\"'#,[]{}:";
    if (kBadLead.find(s.front()) != std::string::npos) return true;
    if (s.front() == '-' && (s.size() == 1 || s[1] == ' ')) return true;
    if (s.front() == ' ' || s.back() == ' ') return true;
    // A ": " or trailing ":" makes a plain scalar ambiguous with a mapping.
    if (s.find(": ") != std::string::npos) return true;
    if (s.back() == ':') return true;
    // Control chars / newlines force double-quote.
    for (char c : s)
        if (static_cast<unsigned char>(c) < 0x20) return true;
    return false;
}

// Emit a scalar node's canonical representation (no key, no newline).
// `tag` is the node's source Tag(): "!" = explicitly a string (was quoted),
// "?" = plain (type inferred), "" = null.
std::string emit_scalar(const YAML::Node& n) {
    const std::string& raw = n.Scalar();
    const std::string tag = n.Tag();

    if (n.IsNull() || tag == "!!null") return "null";
    if (tag.empty() && raw.empty()) return "null";

    // Decide whether this scalar must be quoted. yaml-cpp reports tag "!" for a
    // scalar that was NON-PLAIN in the source (single/double-quoted or a block
    // scalar) — i.e. the author explicitly chose string form. We PRESERVE that
    // choice: an explicitly-quoted scalar stays quoted. This is both safe (a
    // string that looks like a number/bool never silently changes type) and
    // low-churn (the author's `"0.1.0"`, `"4/4"`, `"4.6"` do not lose quotes).
    const bool source_was_string = (tag == "!");
    bool quote = needs_quote_for_safety(raw) || source_was_string;

    if (!quote) return raw;

    // Double-quote with minimal escaping (\, ", control chars).
    std::string out = "\"";
    for (char c : raw) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    static const char* hex = "0123456789abcdef";
                    out += "\\x";
                    out += hex[(c >> 4) & 0xF];
                    out += hex[c & 0xF];
                } else {
                    out += c;
                }
        }
    }
    out += "\"";
    return out;
}

// ---- Key ordering ----------------------------------------------------------
//
// Canonical order: the envelope keys first, in fixed order, then all remaining
// keys in their SOURCE order. Source order is stable and deterministic for a
// given file, and — critically — never reorders keys the author grouped
// meaningfully (a formatter that only fixes the envelope + shape is the safe,
// non-surprising choice; the schema-driven full sort is a documented future
// extension once schemas are loaded, FORMAT.md §Key order).

const std::vector<std::string>& envelope_order() {
    static const std::vector<std::string> kEnvelope = {"schema", "id", "namespace"};
    return kEnvelope;
}

// Return map keys in canonical order: envelope keys (present ones, fixed order)
// first, then the rest in the order yaml-cpp iterates them (source order).
std::vector<std::string> ordered_keys(const YAML::Node& map) {
    std::vector<std::string> source_keys;
    for (auto it = map.begin(); it != map.end(); ++it) {
        if (it->first.IsScalar()) source_keys.push_back(it->first.Scalar());
    }
    std::vector<std::string> out;
    std::vector<bool> used(source_keys.size(), false);
    for (const auto& env : envelope_order()) {
        for (std::size_t i = 0; i < source_keys.size(); ++i) {
            if (!used[i] && source_keys[i] == env) {
                out.push_back(source_keys[i]);
                used[i] = true;
            }
        }
    }
    for (std::size_t i = 0; i < source_keys.size(); ++i)
        if (!used[i]) out.push_back(source_keys[i]);
    return out;
}

// ---- Collection style (flow vs block) -------------------------------------
//
// Canonical rule (FORMAT.md §Collection style): PRESERVE the author's flow-vs-
// block choice, but only allow flow for a "leaf" collection — one whose values
// are all scalars/null. This keeps the house style for compact fixed-shape maps
// (`{ min, max }`, `{ x, y, z }`, `[a, b, c]`) while never squeezing a nested
// structure onto one line. A flow collection authored with a non-scalar child
// is normalized to block. The decision depends only on the parsed tree, so it
// is deterministic and idempotent.

bool all_children_scalar(const YAML::Node& n) {
    if (n.IsMap()) {
        for (auto it = n.begin(); it != n.end(); ++it)
            if (!(it->second.IsScalar() || it->second.IsNull())) return false;
        return true;
    }
    if (n.IsSequence()) {
        for (const auto& e : n)
            if (!(e.IsScalar() || e.IsNull())) return false;
        return true;
    }
    return false;
}

bool emit_as_flow(const YAML::Node& n) {
    if (n.size() == 0) return false;  // {} / [] handled separately
    return n.Style() == YAML::EmitterStyle::Flow && all_children_scalar(n);
}

// ---- The recursive block emitter ------------------------------------------

class Emitter {
public:
    Emitter(std::string& out, CommentIndex& comments) : out_(out), comments_(comments) {}

    // Emit a document root (must be a mapping) at indent 0.
    void emit_document(const YAML::Node& root) {
        emit_map_body(root, 0);
        // File-trailing comments, if any.
        for (const auto& c : comments_.footer()) {
            out_ += c;
            out_ += '\n';
        }
    }

private:
    void indent(int level) { out_.append(static_cast<std::size_t>(level) * kIndentStep, ' '); }

    void emit_leading(const YAML::Node& n, int level) {
        const YAML::Mark m = n.Mark();
        if (m.line < 0) return;
        for (const auto& c : comments_.take_leading(m.line + 1)) {
            indent(level);
            out_ += c;
            out_ += '\n';
        }
    }
    std::string trailing_for(const YAML::Node& n) {
        const YAML::Mark m = n.Mark();
        if (m.line < 0) return "";
        std::string t = comments_.trailing(m.line + 1);
        return t.empty() ? "" : ("  " + t);
    }

    // Emit the body of a mapping (each "key: value" on its own line) at `level`.
    void emit_map_body(const YAML::Node& map, int level) {
        for (const auto& key : ordered_keys(map)) {
            const YAML::Node val = map[key];
            emit_key_value(key, val, level);
        }
    }

    void emit_key_value(const std::string& key, const YAML::Node& val, int level) {
        // Leading comments attach to the value's source line (that is the line
        // the "key:" pair sits on).
        emit_leading(val, level);
        indent(level);
        out_ += emit_key(key);
        out_ += ':';
        emit_value_after_key(val, level);
    }

    // Emit a block sequence's items one indent step under `level`
    // (canonical: "- " markers indented one step under the owning key).
    void emit_seq_body(const YAML::Node& seq, int level) {
        for (std::size_t i = 0; i < seq.size(); ++i) {
            const YAML::Node item = seq[i];
            emit_leading(item, level + 1);
            indent(level + 1);
            out_ += "- ";
            emit_seq_item(item, level + 1);
        }
    }

    // Emit one block-sequence item; the "- " has already been written at `level`.
    void emit_seq_item(const YAML::Node& item, int level) {
        if (item.IsScalar() || item.IsNull()) {
            out_ += emit_scalar(item);
            out_ += trailing_for(item);
            out_ += '\n';
        } else if (item.IsMap() && emit_as_flow(item)) {
            out_ += emit_flow(item);
            out_ += trailing_for(item);
            out_ += '\n';
        } else if (item.IsMap()) {
            // First key sits on the "- " line; the rest indent to align with it.
            bool first = true;
            for (const auto& key : ordered_keys(item)) {
                const YAML::Node val = item[key];
                if (!first) {
                    emit_leading(val, level + 1);
                    indent(level + 1);
                }
                out_ += emit_key(key);
                out_ += ':';
                emit_value_after_key(val, level + 1);
                first = false;
            }
        } else if (item.IsSequence() && emit_as_flow(item)) {
            out_ += emit_flow(item);
            out_ += trailing_for(item);
            out_ += '\n';
        } else if (item.IsSequence()) {
            // Nested block sequence directly under a "-": rare in content.
            out_ += '\n';
            emit_seq_body(item, level);
        } else {
            out_ += "null\n";
        }
    }

    // Emit the value following an already-written "key:" (or "- key:"): either
    // " <scalar>\n", " {flow}\n"/" [flow]\n", or a newline + block body.
    void emit_value_after_key(const YAML::Node& val, int level) {
        if (val.IsScalar() || val.IsNull()) {
            out_ += ' ';
            out_ += emit_scalar(val);
            out_ += trailing_for(val);
            out_ += '\n';
        } else if (val.IsSequence()) {
            if (val.size() == 0) {
                out_ += " []";
            } else if (emit_as_flow(val)) {
                out_ += ' ';
                out_ += emit_flow(val);
            } else {
                out_ += trailing_for(val);
                out_ += '\n';
                emit_seq_body(val, level);
                return;
            }
            out_ += trailing_for(val);
            out_ += '\n';
        } else if (val.IsMap()) {
            if (val.size() == 0) {
                out_ += " {}";
            } else if (emit_as_flow(val)) {
                out_ += ' ';
                out_ += emit_flow(val);
            } else {
                out_ += trailing_for(val);
                out_ += '\n';
                emit_map_body(val, level + 1);
                return;
            }
            out_ += trailing_for(val);
            out_ += '\n';
        } else {
            out_ += " null\n";
        }
    }

    // Render a leaf collection in canonical flow style:
    //   map -> "{ k: v, k: v }"   seq -> "[a, b, c]"
    // Keys are canonically ordered; scalars use the same quoting as block mode.
    std::string emit_flow(const YAML::Node& n) {
        std::string s;
        if (n.IsMap()) {
            s += "{ ";
            bool first = true;
            for (const auto& key : ordered_keys(n)) {
                if (!first) s += ", ";
                s += emit_key(key);
                s += ": ";
                s += emit_scalar(n[key]);
                first = false;
            }
            s += " }";
        } else {  // sequence
            s += "[";
            bool first = true;
            for (const auto& e : n) {
                if (!first) s += ", ";
                s += emit_scalar(e);
                first = false;
            }
            s += "]";
        }
        return s;
    }

    // A mapping key is itself a scalar; quote it under the same rules.
    std::string emit_key(const std::string& key) {
        // Build a transient scalar-like decision: keys are plain unless unsafe.
        if (needs_quote_for_safety(key) || looks_like_non_string(key)) {
            std::string out = "\"";
            for (char c : key) {
                if (c == '\\' || c == '"') out += '\\';
                out += c;
            }
            out += "\"";
            return out;
        }
        return key;
    }

    std::string& out_;
    CommentIndex& comments_;
};

}  // namespace

FormatResult format_yaml(const std::string& input) {
    FormatResult r;
    YAML::Node root;
    try {
        root = YAML::Load(input);
    } catch (const YAML::Exception& ex) {
        r.error = std::string("invalid YAML: ") + ex.what();
        return r;
    }
    if (!root || root.IsNull() || !root.IsMap()) {
        r.error = "document is not a YAML mapping";
        return r;
    }

    CommentIndex comments(input);
    std::string out;
    Emitter em(out, comments);
    em.emit_document(root);

    // Normalize: exactly one trailing newline, no trailing spaces on any line.
    // (emit already avoids trailing spaces, but a final guard is cheap.)
    while (out.size() >= 2 && out[out.size() - 1] == '\n' && out[out.size() - 2] == '\n')
        out.pop_back();
    if (out.empty() || out.back() != '\n') out += '\n';

    r.ok = true;
    r.output = std::move(out);
    return r;
}

namespace {

std::string read_file(const std::string& path, bool& ok) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        ok = false;
        return {};
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    ok = true;
    return ss.str();
}

}  // namespace

int format_file(const std::string& path, bool check_only, std::ostream& out,
                std::ostream& err) {
    bool read_ok = false;
    const std::string input = read_file(path, read_ok);
    if (!read_ok) {
        err << "mcc fmt: cannot read file: " << path << '\n';
        return 2;
    }

    FormatResult fr = format_yaml(input);
    if (!fr.ok) {
        err << "mcc fmt: " << path << ": " << fr.error << '\n';
        return 2;
    }

    if (fr.output == input) return 0;  // already canonical

    if (check_only) {
        out << "would reformat: " << path << '\n';
        return 1;
    }

    std::ofstream w(path, std::ios::binary | std::ios::trunc);
    if (!w) {
        err << "mcc fmt: cannot write file: " << path << '\n';
        return 2;
    }
    w << fr.output;
    out << "formatted: " << path << '\n';
    return 0;
}

int fmt(const std::string& path, bool check_only, std::ostream& out, std::ostream& err) {
    std::error_code ec;
    if (!fs::exists(fs::path(path), ec)) {
        err << "mcc fmt: path not found: " << path << '\n';
        return 2;
    }

    std::vector<std::string> targets;
    if (fs::is_directory(fs::path(path), ec)) {
        for (fs::recursive_directory_iterator it(fs::path(path), ec), end;
             it != end && !ec; it.increment(ec)) {
            if (it->is_regular_file(ec) && it->path().extension() == ".yaml")
                targets.push_back(it->path().generic_string());
        }
        std::sort(targets.begin(), targets.end());
    } else {
        targets.push_back(path);
    }

    if (targets.empty()) {
        out << "mcc fmt: no *.yaml files under " << path << '\n';
        return 0;
    }

    int worst = 0;  // 0 clean, 1 drift (--check), 2 error
    for (const auto& t : targets) {
        const int rc = format_file(t, check_only, out, err);
        if (rc == 2) worst = 2;
        else if (rc == 1 && worst < 1) worst = 1;
    }
    return worst;
}

}  // namespace mcc::stages
