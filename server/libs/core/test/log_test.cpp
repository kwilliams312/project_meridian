// SPDX-License-Identifier: Apache-2.0
//
// libmeridian-core — structured logger unit test (OPS-05, #165).
//
// Self-contained (no DB, no socket): drives the log API directly and checks the
// rendered records. Runs in the plain server ctest.
//
// Coverage:
//   1. A JSON record is a single valid JSON object carrying the required Loki
//      schema fields (realm/process/level/event/severity/logger/message/
//      timestamp_ms) — parsed back and asserted.
//   2. Typed structured fields render as JSON string vs bare number correctly.
//   3. JSON escaping of quotes/backslash/control chars in message + field values.
//   4. Level filtering: a record below the global level is dropped.
//   5. Text mode renders the legacy readable "<ts> LEVEL [category] message" line
//      with key=value field trailers.
//   6. Concurrency: many threads logging JSON to a redirected stdout produce
//      lines that are EACH a complete, independently-parseable JSON object (no
//      interleaving), and all N lines arrive.
//   7. level_from_string / format_from_string / level_loki vocabulary.

#include "meridian/core/log.hpp"

#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace log = meridian::core::log;

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

bool has(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

// -------------------------------------------------------------------------
// Minimal JSON object parser — just enough to validate that a log line is a
// well-formed JSON object and to pull out string/number member values. Not a
// general parser; it handles the flat {"k":"v","k2":123,...} shape the logger
// emits (strings with standard escapes, integer numbers, no nesting).
// Returns false if the text is not a valid object of that shape.
// -------------------------------------------------------------------------
struct Json {
    std::vector<std::pair<std::string, std::string>> members;  // value stored raw
    std::vector<bool> is_string;                                // parallel: was quoted?

    const std::string* get(const std::string& key) const {
        for (std::size_t i = 0; i < members.size(); ++i) {
            if (members[i].first == key) return &members[i].second;
        }
        return nullptr;
    }
    bool key_is_string(const std::string& key) const {
        for (std::size_t i = 0; i < members.size(); ++i) {
            if (members[i].first == key) return is_string[i];
        }
        return false;
    }
};

// Parse a JSON string starting at s[i] (s[i]=='"'); decode escapes into `out`;
// advance i past the closing quote. Returns false on malformed input.
bool parse_string(const std::string& s, std::size_t& i, std::string& out) {
    if (i >= s.size() || s[i] != '"') return false;
    ++i;
    while (i < s.size()) {
        char c = s[i++];
        if (c == '"') return true;
        if (c == '\\') {
            if (i >= s.size()) return false;
            char e = s[i++];
            switch (e) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    if (i + 4 > s.size()) return false;
                    // Only \u00XX control escapes are produced by the logger.
                    unsigned code = 0;
                    for (int k = 0; k < 4; ++k) {
                        char h = s[i++];
                        code <<= 4;
                        if (h >= '0' && h <= '9') code |= static_cast<unsigned>(h - '0');
                        else if (h >= 'a' && h <= 'f') code |= static_cast<unsigned>(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') code |= static_cast<unsigned>(h - 'A' + 10);
                        else return false;
                    }
                    out.push_back(static_cast<char>(code & 0xFF));
                    break;
                }
                default: return false;
            }
        } else {
            out.push_back(c);
        }
    }
    return false;  // unterminated
}

bool parse_object(const std::string& s, Json& out) {
    std::size_t i = 0;
    auto skip_ws = [&] { while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i; };
    skip_ws();
    if (i >= s.size() || s[i] != '{') return false;
    ++i;
    skip_ws();
    if (i < s.size() && s[i] == '}') { ++i; return i == s.size() || s[i] == '\n'; }
    for (;;) {
        skip_ws();
        std::string key;
        if (!parse_string(s, i, key)) return false;
        skip_ws();
        if (i >= s.size() || s[i] != ':') return false;
        ++i;
        skip_ws();
        std::string val;
        bool is_str = false;
        if (i < s.size() && s[i] == '"') {
            if (!parse_string(s, i, val)) return false;
            is_str = true;
        } else if (s.compare(i, 4, "true") == 0) {
            val = "true"; i += 4;             // bare boolean literal
        } else if (s.compare(i, 5, "false") == 0) {
            val = "false"; i += 5;
        } else {
            // number (integer, optional minus) — read run of [-0-9]
            std::size_t start = i;
            if (i < s.size() && s[i] == '-') ++i;
            while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
            if (i == start) return false;
            val = s.substr(start, i - start);
        }
        out.members.emplace_back(key, val);
        out.is_string.push_back(is_str);
        skip_ws();
        if (i < s.size() && s[i] == ',') { ++i; continue; }
        if (i < s.size() && s[i] == '}') { ++i; break; }
        return false;
    }
    // Trailing may be end or newline only.
    return i == s.size() || s[i] == '\n';
}

// -------------------------------------------------------------------------
// Test 1+2+3: JSON render shape, required fields, typed fields, escaping.
// -------------------------------------------------------------------------
int test_json_shape() {
    log::Fields fields = {
        log::field("session_id", std::string("sess-42")),
        log::field("opcode", 7),                       // number
        log::field("grant_id", std::uint64_t{99887766}),  // number
        log::field("ok", true),                        // bare true
    };
    std::string line = log::render_json(log::Level::Info, "world",
                                        "session \"entered\"\tzone", fields);

    Json j;
    CHECK(parse_object(line, j));  // single valid JSON object

    // Required Loki schema fields (match telemetryd #167).
    const std::string* realm = j.get("realm");
    const std::string* process = j.get("process");
    const std::string* level = j.get("level");
    const std::string* event = j.get("event");
    const std::string* severity = j.get("severity");
    const std::string* logger = j.get("logger");
    const std::string* message = j.get("message");
    const std::string* ts = j.get("timestamp_ms");
    CHECK(realm && process && level && event && severity && logger && message && ts);

    CHECK(*level == "info");
    CHECK(*severity == "info");
    CHECK(*event == "world");   // event == subsystem stream name
    CHECK(*logger == "world");
    // message round-trips through escaping (embedded quote + tab decoded back).
    CHECK(*message == "session \"entered\"\tzone");
    // timestamp_ms is a bare number, not a quoted string.
    CHECK(!j.key_is_string("timestamp_ms"));
    CHECK(std::stoull(*ts) > 0);

    // Typed fields.
    const std::string* sid = j.get("session_id");
    const std::string* op = j.get("opcode");
    const std::string* gid = j.get("grant_id");
    const std::string* ok = j.get("ok");
    CHECK(sid && *sid == "sess-42" && j.key_is_string("session_id"));
    CHECK(op && *op == "7" && !j.key_is_string("opcode"));       // bare number
    CHECK(gid && *gid == "99887766" && !j.key_is_string("grant_id"));
    CHECK(ok && *ok == "true" && !j.key_is_string("ok"));
    return 0;
}

// -------------------------------------------------------------------------
// Test 4: level filtering — a call below the global level emits nothing.
// -------------------------------------------------------------------------
int test_level_filter() {
    // render_* ignores level filtering (they're pure renderers); the filter
    // lives in write(). Drive write() with a redirected stdout and count lines.
    log::set_format(log::Format::Json);
    log::set_level(log::Level::Warn);

    std::fflush(stdout);
    char tmpl[] = "/tmp/meridian-log-lvl-XXXXXX";
    int fd = mkstemp(tmpl);
    CHECK(fd >= 0);
    int saved = dup(fileno(stdout));
    CHECK(saved >= 0);
    CHECK(dup2(fd, fileno(stdout)) >= 0);

    log::debug("world", "dropped-debug");  // below Warn -> dropped
    log::info("world", "dropped-info");    // below Warn -> dropped
    log::warn("world", "kept-warn");       // >= Warn -> emitted
    log::error("world", "kept-error");     // >= Warn -> emitted
    std::fflush(stdout);

    CHECK(dup2(saved, fileno(stdout)) >= 0);
    close(saved);
    close(fd);

    std::FILE* f = std::fopen(tmpl, "rb");
    CHECK(f != nullptr);
    std::string content;
    char buf[4096];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) content.append(buf, n);
    std::fclose(f);
    std::remove(tmpl);

    CHECK(!has(content, "dropped-debug"));
    CHECK(!has(content, "dropped-info"));
    CHECK(has(content, "kept-warn"));
    CHECK(has(content, "kept-error"));

    // Reset for later tests.
    log::set_level(log::Level::Info);
    return 0;
}

// -------------------------------------------------------------------------
// Test 5: text mode renders the legacy readable line + key=value trailers.
// -------------------------------------------------------------------------
int test_text_mode() {
    log::Fields fields = {log::field("slot", 3), log::field("guid", std::string("g-7"))};
    std::string line = log::render_text(log::Level::Warn, "authd",
                                        "login rejected", fields);
    CHECK(has(line, "WARN"));
    CHECK(has(line, "[authd]"));
    CHECK(has(line, "login rejected"));
    CHECK(has(line, "slot=3"));
    CHECK(has(line, "guid=g-7"));
    // Not JSON — no leading brace.
    CHECK(line.empty() || line[0] != '{');
    return 0;
}

// -------------------------------------------------------------------------
// Test 6: concurrency — N threads logging JSON to a redirected stdout produce
// N lines, each an independently-parseable complete JSON object (no interleave).
// -------------------------------------------------------------------------
int test_concurrency() {
    log::set_format(log::Format::Json);
    log::set_level(log::Level::Info);

    std::fflush(stdout);
    char tmpl[] = "/tmp/meridian-log-conc-XXXXXX";
    int fd = mkstemp(tmpl);
    CHECK(fd >= 0);
    int saved = dup(fileno(stdout));
    CHECK(saved >= 0);
    CHECK(dup2(fd, fileno(stdout)) >= 0);

    constexpr int kThreads = 8;
    constexpr int kPerThread = 200;
    std::vector<std::thread> ts;
    ts.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        ts.emplace_back([t] {
            for (int k = 0; k < kPerThread; ++k) {
                log::info("world", "concurrent line with \"quotes\" and, commas",
                          {log::field("thread", t), log::field("seq", k)});
            }
        });
    }
    for (auto& th : ts) th.join();
    std::fflush(stdout);

    CHECK(dup2(saved, fileno(stdout)) >= 0);
    close(saved);
    close(fd);

    std::FILE* f = std::fopen(tmpl, "rb");
    CHECK(f != nullptr);
    std::string content;
    char buf[8192];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) content.append(buf, n);
    std::fclose(f);
    std::remove(tmpl);

    // Every non-empty line must be a complete, valid JSON object.
    int lines = 0;
    std::size_t start = 0;
    while (start < content.size()) {
        std::size_t nl = content.find('\n', start);
        std::string ln = (nl == std::string::npos)
                             ? content.substr(start)
                             : content.substr(start, nl - start);
        start = (nl == std::string::npos) ? content.size() : nl + 1;
        if (ln.empty()) continue;
        ++lines;
        Json j;
        if (!parse_object(ln, j)) {
            std::fprintf(stderr, "FAIL: interleaved/malformed line: %s\n", ln.c_str());
            return 1;
        }
        // Sanity: it carries the schema.
        if (!j.get("message") || !j.get("thread") || !j.get("seq")) {
            std::fprintf(stderr, "FAIL: line missing fields: %s\n", ln.c_str());
            return 1;
        }
    }
    CHECK(lines == kThreads * kPerThread);  // no lines lost or merged
    return 0;
}

// -------------------------------------------------------------------------
// Test 7: string-parse vocabulary.
// -------------------------------------------------------------------------
int test_vocab() {
    CHECK(log::level_from_string("TRACE") == log::Level::Trace);
    CHECK(log::level_from_string("Debug") == log::Level::Debug);
    CHECK(log::level_from_string("info") == log::Level::Info);
    CHECK(log::level_from_string("warning") == log::Level::Warn);
    CHECK(log::level_from_string("error") == log::Level::Error);
    CHECK(log::level_from_string("bogus") == log::Level::Info);  // default

    CHECK(log::format_from_string("json") == log::Format::Json);
    CHECK(log::format_from_string("TEXT") == log::Format::Text);
    CHECK(log::format_from_string("bogus") == log::Format::Json);  // default

    CHECK(std::string(log::level_loki(log::Level::Info)) == "info");
    CHECK(std::string(log::level_loki(log::Level::Error)) == "error");
    CHECK(std::string(log::level_name(log::Level::Warn)) == "WARN");
    return 0;
}

}  // namespace

int main() {
    if (test_json_shape() != 0) return 1;
    if (test_level_filter() != 0) return 1;
    if (test_text_mode() != 0) return 1;
    if (test_concurrency() != 0) return 1;
    if (test_vocab() != 0) return 1;
    std::printf("OK meridian-core log: %d checks passed\n", g_checks);
    return 0;
}
