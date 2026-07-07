// SPDX-License-Identifier: Apache-2.0
//
// libmeridian-core — layered configuration loader unit test (issue #90).
//
// Self-contained (no DB, no socket): drives the loader API directly and checks
// the resolved values. Runs in the plain server ctest.
//
// Coverage:
//   1. Precedence — a key present in all four layers resolves to CommandLine;
//      File overrides Default; Environment overrides File; CommandLine overrides
//      Environment; a lower layer can NEVER clobber a higher one regardless of
//      load order.
//   2. Typed getters + fallbacks (get_int / get_bool / *_or, missing-key).
//   3. File parser — sections (incl. dotted), dotted keys, quote stripping,
//      comments, blank lines, "[]" reset.
//   4. Malformed-file handling — a bad line yields ok=false + a 1-based line no,
//      and well-formed keys before it still apply.
//   5. Missing file — reported (file_found=false), NOT an error.
//   6. Env prefix mapping — env_name_to_key rule + load_env_prefixed over an
//      injected environ (prefix filtering, lowercase, '_' -> '.').
//   7. Command-line "--key=value" ingestion (load_args_kv), form filtering.

#include "meridian/core/config.hpp"
#include "meridian/core/config_loader.hpp"

#include <cstdio>
#include <string>

namespace cfgl = meridian::core;

namespace {

int g_checks = 0;
#define CHECK(cond)                                                             \
    do {                                                                        \
        ++g_checks;                                                             \
        if (!(cond)) {                                                          \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);\
            return 1;                                                           \
        }                                                                       \
    } while (0)

using cfgl::Config;
using cfgl::ConfigLayer;

// -------------------------------------------------------------------------
// Test 1: four-layer precedence + order-independence.
// -------------------------------------------------------------------------
int test_precedence() {
    // A key written by every layer resolves to the CommandLine value. Deliberately
    // apply the layers OUT of precedence order to prove Config::set enforces the
    // ordering by layer, not by write order.
    Config c;
    c.set("port", "1", ConfigLayer::CommandLine);   // highest, written first
    c.set("port", "2", ConfigLayer::Environment);   // must NOT override
    c.set("port", "3", ConfigLayer::File);          // must NOT override
    c.set("port", "4", ConfigLayer::Default);       // must NOT override
    CHECK(c.get_string_or("port", "?") == "1");

    // File overrides Default.
    Config c2;
    c2.set("bind", "0.0.0.0", ConfigLayer::Default);
    c2.set("bind", "10.0.0.1", ConfigLayer::File);
    CHECK(c2.get_string_or("bind", "?") == "10.0.0.1");

    // Environment overrides File.
    Config c3;
    c3.set("db.host", "file-host", ConfigLayer::File);
    c3.set("db.host", "env-host", ConfigLayer::Environment);
    CHECK(c3.get_string_or("db.host", "?") == "env-host");

    // CommandLine overrides Environment.
    Config c4;
    c4.set("db.host", "env-host", ConfigLayer::Environment);
    c4.set("db.host", "cli-host", ConfigLayer::CommandLine);
    CHECK(c4.get_string_or("db.host", "?") == "cli-host");

    // A lower layer arriving LATER is ignored (the loaders rely on this so a
    // File parsed after Env/CLI cannot clobber them).
    Config c5;
    c5.set("realm", "cli", ConfigLayer::CommandLine);
    c5.set("realm", "default", ConfigLayer::Default);  // late, lower -> ignored
    CHECK(c5.get_string_or("realm", "?") == "cli");
    return 0;
}

// -------------------------------------------------------------------------
// Test 2: typed getters + fallbacks + missing key.
// -------------------------------------------------------------------------
int test_typed_getters() {
    Config c;
    c.set("port", "7100");
    c.set("enabled", "true");
    c.set("bad_int", "12x");

    CHECK(c.get_int("port").has_value() && *c.get_int("port") == 7100);
    CHECK(c.get_int_or("port", -1) == 7100);
    CHECK(c.get_bool("enabled").has_value() && *c.get_bool("enabled") == true);
    CHECK(!c.get_int("bad_int").has_value());        // unparseable -> nullopt
    CHECK(c.get_int_or("bad_int", 42) == 42);        // falls back

    // Missing keys fall back.
    CHECK(!c.contains("absent"));
    CHECK(c.get_string_or("absent", "fallback") == "fallback");
    CHECK(c.get_int_or("absent", 99) == 99);
    CHECK(c.get_bool_or("absent", true) == true);
    return 0;
}

// -------------------------------------------------------------------------
// Test 3: file (string) parser — sections, dotted keys, quotes, comments.
// -------------------------------------------------------------------------
int test_file_parser() {
    const char* text =
        "# Meridian config\n"
        "; alt comment style\n"
        "\n"
        "realm = \"reference\"        \n"     // trailing spaces, double quotes
        "[db]\n"
        "host = 127.0.0.1\n"
        "port = 3306\n"
        "pass = 'p@ss#word'\n"                // single quotes; '#' kept in value
        "[metrics]\n"
        "bind = 127.0.0.1\n"
        "a.b = nested\n"                       // dotted key inside a section
        "[]\n"                                  // reset to top level
        "top = yes\n";

    Config c;
    cfgl::FileLoadResult r = cfgl::load_config_string(c, text);
    CHECK(r.ok);
    CHECK(r.file_found);
    CHECK(r.keys_loaded == 7);

    CHECK(c.get_string_or("realm", "?") == "reference");     // quotes stripped
    CHECK(c.get_string_or("db.host", "?") == "127.0.0.1");
    CHECK(c.get_int_or("db.port", -1) == 3306);
    CHECK(c.get_string_or("db.pass", "?") == "p@ss#word");   // '#' preserved
    CHECK(c.get_string_or("metrics.bind", "?") == "127.0.0.1");
    CHECK(c.get_string_or("metrics.a.b", "?") == "nested");  // section + dotted key
    CHECK(c.get_string_or("top", "?") == "yes");             // after "[]" reset

    // The file layer is ConfigLayer::File — a Default write cannot override, an
    // Environment write can.
    c.set("realm", "should-not-win", ConfigLayer::Default);
    CHECK(c.get_string_or("realm", "?") == "reference");
    c.set("realm", "env-wins", ConfigLayer::Environment);
    CHECK(c.get_string_or("realm", "?") == "env-wins");
    return 0;
}

// -------------------------------------------------------------------------
// Test 4: malformed file — reports the bad line; earlier keys still apply.
// -------------------------------------------------------------------------
int test_malformed_file() {
    const char* text =
        "good = 1\n"
        "also_good = 2\n"
        "this line has no equals sign\n"   // line 3: malformed
        "never = seen\n";

    Config c;
    cfgl::FileLoadResult r = cfgl::load_config_string(c, text);
    CHECK(!r.ok);
    CHECK(r.error_line == 3);
    CHECK(!r.error.empty());
    // Keys before the error were applied; the one after was not.
    CHECK(c.get_int_or("good", -1) == 1);
    CHECK(c.get_int_or("also_good", -1) == 2);
    CHECK(!c.contains("never"));

    // Unterminated section header is also flagged.
    Config c2;
    cfgl::FileLoadResult r2 = cfgl::load_config_string(c2, "[oops\nkey = v\n");
    CHECK(!r2.ok);
    CHECK(r2.error_line == 1);
    return 0;
}

// -------------------------------------------------------------------------
// Test 5: missing file is not an error.
// -------------------------------------------------------------------------
int test_missing_file() {
    Config c;
    cfgl::FileLoadResult r =
        cfgl::load_config_file(c, "/nonexistent/meridian-does-not-exist.toml");
    CHECK(r.ok);              // not an error
    CHECK(!r.file_found);     // but the file was absent
    CHECK(r.keys_loaded == 0);
    // Empty path is likewise a no-op.
    cfgl::FileLoadResult r2 = cfgl::load_config_file(c, "");
    CHECK(r2.ok);
    CHECK(!r2.file_found);
    return 0;
}

// -------------------------------------------------------------------------
// Test 6: environment prefix mapping.
// -------------------------------------------------------------------------
int test_env_mapping() {
    // env_name_to_key rule.
    CHECK(cfgl::env_name_to_key("MERIDIAN_DB_HOST") == "db.host");
    CHECK(cfgl::env_name_to_key("MERIDIAN_METRICS_PORT") == "metrics.port");
    CHECK(cfgl::env_name_to_key("MERIDIAN_REALM") == "realm");
    CHECK(cfgl::env_name_to_key("MERIDIAN_WORLDDB_EXPECTED_HASH") ==
          "worlddb.expected.hash");
    CHECK(cfgl::env_name_to_key("PATH").empty());          // wrong prefix
    CHECK(cfgl::env_name_to_key("MERIDIAN_").empty());      // empty remainder
    CHECK(cfgl::env_name_to_key("DB_HOST", "DB_") == "host");  // custom prefix

    // load_env_prefixed over an injected environ (no real getenv mutation).
    const char* fake_environ[] = {
        "MERIDIAN_DB_HOST=db.example",
        "MERIDIAN_METRICS_PORT=9500",
        "MERIDIAN_REALM=nightly",
        "PATH=/usr/bin",                 // ignored (wrong prefix)
        "NOTMERIDIAN_X=y",               // ignored (wrong prefix)
        "MALFORMED_NO_EQUALS",           // ignored (no '=')  -> actually has none
        nullptr,
    };
    Config c;
    std::size_t n = cfgl::load_env_prefixed(c, "MERIDIAN_", fake_environ);
    CHECK(n == 3);
    CHECK(c.get_string_or("db.host", "?") == "db.example");
    CHECK(c.get_int_or("metrics.port", -1) == 9500);
    CHECK(c.get_string_or("realm", "?") == "nightly");
    CHECK(!c.contains("path"));

    // These land at Environment layer: a File write cannot override, CommandLine can.
    c.set("realm", "file", ConfigLayer::File);
    CHECK(c.get_string_or("realm", "?") == "nightly");
    c.set("realm", "cli", ConfigLayer::CommandLine);
    CHECK(c.get_string_or("realm", "?") == "cli");
    return 0;
}

// -------------------------------------------------------------------------
// Test 7: command-line "--key=value" ingestion.
// -------------------------------------------------------------------------
int test_args_kv() {
    const char* argv[] = {
        "authd",                    // argv[0] program name -> ignored
        "--port=7101",
        "--db.host=cli-host",
        "--metrics.bind=0.0.0.0",
        "--cert",                   // named "--flag value" form -> NOT consumed here
        "/path/to/cert.pem",
        "--=novalue",               // empty key -> skipped
        "-p=x",                     // single dash -> skipped
        "positional",               // not a flag -> skipped
    };
    const int argc = static_cast<int>(sizeof(argv) / sizeof(argv[0]));

    Config c;
    std::size_t n = cfgl::load_args_kv(c, argc, argv);
    CHECK(n == 3);
    CHECK(c.get_int_or("port", -1) == 7101);
    CHECK(c.get_string_or("db.host", "?") == "cli-host");
    CHECK(c.get_string_or("metrics.bind", "?") == "0.0.0.0");
    CHECK(!c.contains("cert"));     // the "--flag value" form was left alone
    // Ingested at CommandLine layer -> nothing overrides it.
    c.set("port", "9999", ConfigLayer::Environment);
    CHECK(c.get_int_or("port", -1) == 7101);
    return 0;
}

// -------------------------------------------------------------------------
// Test 8: end-to-end assembly mirroring a daemon's four-layer build.
// -------------------------------------------------------------------------
int test_end_to_end_assembly() {
    Config c;
    // Default layer (in-code).
    c.set("bind", "0.0.0.0", ConfigLayer::Default);
    c.set("port", "7100", ConfigLayer::Default);
    c.set("metrics.port", "9464", ConfigLayer::Default);
    c.set("realm", "reference", ConfigLayer::Default);

    // Environment layer.
    const char* env[] = {
        "MERIDIAN_REALM=nightly",
        "MERIDIAN_METRICS_PORT=9500",
        "MERIDIAN_DB_HOST=db.internal",
        nullptr,
    };
    cfgl::load_env_prefixed(c, "MERIDIAN_", env);

    // Command-line layer.
    const char* argv[] = {"authd", "--port=7200", "--realm=live"};
    cfgl::load_args_kv(c, 3, argv);

    // File layer applied LAST (out of order) — must not clobber env/cli.
    const char* file =
        "port = 5000\n"       // loses to cli 7200
        "realm = filerealm\n" // loses to cli 'live'
        "bind = 192.168.1.1\n";  // only default present below -> file wins
    cfgl::load_config_string(c, file);

    CHECK(c.get_int_or("port", -1) == 7200);          // CommandLine
    CHECK(c.get_string_or("realm", "?") == "live");   // CommandLine
    CHECK(c.get_int_or("metrics.port", -1) == 9500);  // Environment
    CHECK(c.get_string_or("db.host", "?") == "db.internal");  // Environment
    CHECK(c.get_string_or("bind", "?") == "192.168.1.1");    // File over Default
    return 0;
}

}  // namespace

int main() {
    if (test_precedence() != 0) return 1;
    if (test_typed_getters() != 0) return 1;
    if (test_file_parser() != 0) return 1;
    if (test_malformed_file() != 0) return 1;
    if (test_missing_file() != 0) return 1;
    if (test_env_mapping() != 0) return 1;
    if (test_args_kv() != 0) return 1;
    if (test_end_to_end_assembly() != 0) return 1;
    std::printf("OK meridian-core config: %d checks passed\n", g_checks);
    return 0;
}
