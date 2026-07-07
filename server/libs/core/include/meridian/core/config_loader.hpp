// SPDX-License-Identifier: Apache-2.0
//
// libmeridian-core — layered configuration LOADER (issue #90).
//
// Clean-room: designed from the server PRD §"Config (OPS-01)" — "layered —
// compiled defaults <- /etc/meridian/*.toml <- env-var overrides (12-factor for
// containers)" — extended with a highest-precedence command-line layer per the
// issue. No GPL source consulted. See CONTRIBUTING.md.
//
// This is the REAL loader that fills a `Config` (config.hpp) from the four
// sources, in the precedence the ConfigLayer enum encodes:
//
//     Default  <  File  <  Environment  <  CommandLine        (highest wins)
//
// Because Config::set only lets an equal-or-higher layer overwrite an existing
// value, the loaders may run in ANY order and the precedence still resolves —
// e.g. a File value can never clobber an Environment or CommandLine value even
// if the file is parsed last. Callers therefore need not sequence the layers.
//
// The file format is a deliberately tiny, dependency-free TOML/INI subset (no
// third-party parser — libmeridian-core stays zero-dependency at M0):
//
//     # a comment (';' also starts a comment); blank lines are ignored
//     realm = "reference"          # bare key -> key "realm"
//     [db]                         # section -> keys are prefixed "db."
//     host = 127.0.0.1             # -> key "db.host"
//     port = 3306                  # -> key "db.port"
//     [metrics]
//     bind = "127.0.0.1"           # quotes (either kind) are stripped
//
// Section headers may be dotted ([a.b] -> prefix "a.b."); keys may be dotted
// too. Values are trimmed; a single layer of surrounding "double" or 'single'
// quotes is removed. '#'/' ;' only start a comment at the FIRST non-space column
// of a line (no inline comments) so values may safely contain '#'.

#ifndef MERIDIAN_CORE_CONFIG_LOADER_HPP
#define MERIDIAN_CORE_CONFIG_LOADER_HPP

#include "meridian/core/config.hpp"

#include <cstddef>
#include <string>
#include <string_view>

namespace meridian::core {

// Outcome of parsing a config file (or string). `ok` is false only for a
// genuine parse error (a malformed line); a MISSING file is NOT an error
// (`ok=true, file_found=false`) so a daemon can point at an optional path and
// fall back to defaults + env + flags when it is absent (12-factor friendly).
struct FileLoadResult {
    bool ok = true;             // false => a line failed to parse
    bool file_found = false;    // false => path did not exist / could not open
    std::string error;          // human-readable reason when ok == false
    std::size_t error_line = 0; // 1-based line number of the first bad line
    std::size_t keys_loaded = 0;// number of key/value pairs applied
};

// Parse a config file at `path` into `cfg` at ConfigLayer::File. A missing file
// is reported (file_found=false) but is not an error. On a malformed line the
// parse STOPS at that line: `ok=false` with `error`/`error_line`, and the
// well-formed keys seen before it remain applied (best-effort overlay).
FileLoadResult load_config_file(Config& cfg, const std::string& path);

// Same parser over an in-memory string (no filesystem). Used by the file loader
// and directly by tests. `file_found` is always true for the string overload.
FileLoadResult load_config_string(Config& cfg, std::string_view text,
                                  ConfigLayer layer = ConfigLayer::File);

// Map a single environment variable NAME to a config key using the documented
// rule: require the `prefix` (default "MERIDIAN_"); strip it; lowercase the
// remainder; replace every '_' with '.'. Returns an empty string when `name`
// does not start with `prefix` (case-sensitive) or the remainder is empty.
//
//   MERIDIAN_DB_HOST      -> "db.host"
//   MERIDIAN_METRICS_PORT -> "metrics.port"
//   MERIDIAN_REALM        -> "realm"
std::string env_name_to_key(std::string_view name, std::string_view prefix = "MERIDIAN_");

// Ingest every environment variable named "<prefix>..." into `cfg` at
// ConfigLayer::Environment, keyed via env_name_to_key(). `environ_override`,
// when non-null, is a NULL-terminated "KEY=VALUE" array used INSTEAD of the
// process environment (so tests need not mutate the real environ). Returns the
// number of variables ingested.
std::size_t load_env_prefixed(Config& cfg, std::string_view prefix = "MERIDIAN_",
                              const char* const* environ_override = nullptr);

// Parse "--key=value" tokens from argv into `cfg` at ConfigLayer::CommandLine.
// Only the "--key=value" form is consumed (the daemons keep their own named
// "--flag value" parsers for legacy flags); any token that is not "--k=v" is
// ignored here. An empty key (e.g. "--=x") is skipped. Returns the count applied.
std::size_t load_args_kv(Config& cfg, int argc, const char* const* argv);

}  // namespace meridian::core

#endif  // MERIDIAN_CORE_CONFIG_LOADER_HPP
