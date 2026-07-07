// SPDX-License-Identifier: Apache-2.0
//
// libmeridian-core — layered configuration loader implementation (issue #90).
// Clean-room per CONTRIBUTING.md; zero third-party dependencies.

#include "meridian/core/config_loader.hpp"

#include <cctype>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

// POSIX process environment. Declared here (rather than relying on a header that
// may or may not expose it) so load_env_prefixed can walk it when no override is
// supplied. Valid for a static library linked into an executable.
extern "C" char** environ;

namespace meridian::core {

namespace {

// Trim ASCII whitespace from both ends of a view, returning a sub-view.
std::string_view trim(std::string_view s) {
    std::size_t b = 0;
    std::size_t e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

// Remove ONE layer of matching surrounding quotes ("..." or '...'). A value with
// only a leading quote, or mismatched quotes, is left untouched.
std::string_view unquote(std::string_view s) {
    if (s.size() >= 2) {
        char q = s.front();
        if ((q == '"' || q == '\'') && s.back() == q) {
            return s.substr(1, s.size() - 2);
        }
    }
    return s;
}

}  // namespace

FileLoadResult load_config_string(Config& cfg, std::string_view text, ConfigLayer layer) {
    FileLoadResult r;
    r.file_found = true;

    std::string section;  // current "[section]" prefix, e.g. "db." (empty = none)
    std::size_t line_no = 0;

    std::size_t pos = 0;
    while (pos <= text.size()) {
        // Slice out the next line (without the trailing '\n').
        std::size_t nl = text.find('\n', pos);
        std::string_view raw =
            (nl == std::string_view::npos) ? text.substr(pos) : text.substr(pos, nl - pos);
        pos = (nl == std::string_view::npos) ? text.size() + 1 : nl + 1;
        ++line_no;

        std::string_view line = trim(raw);
        if (line.empty()) continue;
        if (line.front() == '#' || line.front() == ';') continue;  // comment

        // Section header: [name] (may be dotted). Sets the key prefix.
        if (line.front() == '[') {
            if (line.back() != ']') {
                r.ok = false;
                r.error = "unterminated section header";
                r.error_line = line_no;
                return r;
            }
            std::string_view name = trim(line.substr(1, line.size() - 2));
            if (name.empty()) {
                section.clear();  // "[]" resets to the top level
            } else {
                section.assign(name);
                section.push_back('.');
            }
            continue;
        }

        // key = value
        std::size_t eq = line.find('=');
        if (eq == std::string_view::npos) {
            r.ok = false;
            r.error = "expected 'key = value'";
            r.error_line = line_no;
            return r;
        }
        std::string_view key = trim(line.substr(0, eq));
        if (key.empty()) {
            r.ok = false;
            r.error = "empty key";
            r.error_line = line_no;
            return r;
        }
        std::string_view val = unquote(trim(line.substr(eq + 1)));

        std::string full_key = section;
        full_key.append(key);
        cfg.set(full_key, val, layer);
        ++r.keys_loaded;
    }

    return r;
}

FileLoadResult load_config_file(Config& cfg, const std::string& path) {
    FileLoadResult r;
    if (path.empty()) return r;  // no path => nothing to load, not an error

    std::ifstream in(path, std::ios::binary);
    if (!in) return r;  // file_found stays false; not an error

    std::ostringstream buf;
    buf << in.rdbuf();
    std::string text = buf.str();

    FileLoadResult sr = load_config_string(cfg, text, ConfigLayer::File);
    sr.file_found = true;  // the path existed and opened
    return sr;
}

std::string env_name_to_key(std::string_view name, std::string_view prefix) {
    if (name.size() <= prefix.size()) return {};
    if (name.substr(0, prefix.size()) != prefix) return {};
    std::string key;
    key.reserve(name.size() - prefix.size());
    for (std::size_t i = prefix.size(); i < name.size(); ++i) {
        char c = name[i];
        if (c == '_') {
            key.push_back('.');
        } else {
            key.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
    }
    return key;
}

std::size_t load_env_prefixed(Config& cfg, std::string_view prefix,
                              const char* const* environ_override) {
    const char* const* e = environ_override ? environ_override
                                            : static_cast<const char* const*>(environ);
    if (e == nullptr) return 0;

    std::size_t count = 0;
    for (; *e != nullptr; ++e) {
        std::string_view entry(*e);
        std::size_t eq = entry.find('=');
        if (eq == std::string_view::npos) continue;  // malformed environ entry
        std::string_view name = entry.substr(0, eq);
        std::string_view value = entry.substr(eq + 1);
        std::string key = env_name_to_key(name, prefix);
        if (key.empty()) continue;  // not our prefix
        cfg.set(key, value, ConfigLayer::Environment);
        ++count;
    }
    return count;
}

std::size_t load_args_kv(Config& cfg, int argc, const char* const* argv) {
    std::size_t count = 0;
    for (int i = 0; i < argc; ++i) {
        std::string_view tok(argv[i]);
        if (tok.size() < 3 || tok.substr(0, 2) != "--") continue;  // need "--x"
        std::string_view body = tok.substr(2);
        std::size_t eq = body.find('=');
        if (eq == std::string_view::npos) continue;  // not the --key=value form
        std::string_view key = body.substr(0, eq);
        if (key.empty()) continue;
        std::string_view value = body.substr(eq + 1);
        cfg.set(key, value, ConfigLayer::CommandLine);
        ++count;
    }
    return count;
}

}  // namespace meridian::core
