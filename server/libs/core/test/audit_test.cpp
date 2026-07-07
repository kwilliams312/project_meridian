// SPDX-License-Identifier: Apache-2.0
//
// libmeridian-core — structured AUDIT stream unit test (OPS-05, #92).
//
// Self-contained (no DB, no socket): drives the audit API directly and checks the
// rendered records. Runs in the plain server ctest.
//
// Coverage:
//   1. An audit record renders as a single valid JSON object carrying the audit
//      stream tag (event/logger="audit" labels + body stream="audit") plus the
//      required schema fields (action/outcome + the log backbone realm/process/
//      level/severity/message/timestamp_ms) — parsed back and asserted.
//   2. A login-FAILURE audit record contains the failure reason AND the actor,
//      but NO secret material — the attempted password / SRP verifier / session
//      key strings are asserted ABSENT from the rendered line (the #92 no-secrets
//      guarantee). Failures render at severity "warn".
//   3. A grant-REJECT audit record carries the machine-readable reject reason
//      code and the correlation id (grant_id), and no secret material.
//   4. Optional fields are omitted when empty/zero (compact records): an
//      unauthenticated failure has no account_id; a success carries no reason.
//   5. Action + outcome vocabulary is the stable expected set.

#include "meridian/core/audit.hpp"
#include "meridian/core/log.hpp"

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace audit = meridian::core::audit;
namespace mlog = meridian::core::log;

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
// Minimal flat-JSON-object parser (same shape the logger emits: strings with
// standard escapes, integer numbers, bare booleans, no nesting). Mirrors the one
// in log_test.cpp — just enough to validate a line and pull member values.
// -------------------------------------------------------------------------
struct Json {
    std::vector<std::pair<std::string, std::string>> members;
    std::vector<bool> is_string;

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
    return false;
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
            val = "true"; i += 4;
        } else if (s.compare(i, 5, "false") == 0) {
            val = "false"; i += 5;
        } else {
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
    return i == s.size() || s[i] == '\n';
}

// -------------------------------------------------------------------------
// Test 1: audit record shape — stream tag + required schema + log backbone.
// -------------------------------------------------------------------------
int test_audit_shape() {
    audit::Record rec;
    rec.action = audit::Action::kSessionEnter;
    rec.outcome = audit::Outcome::kSuccess;
    rec.account_id = 42;
    rec.target = "Ragnaros";            // character name — not a secret
    rec.correlation_id = 99887766;      // grant_id
    rec.peer = "10.0.0.5:51000";

    std::string line = audit::render_json(rec);

    Json j;
    CHECK(parse_object(line, j));  // single valid JSON object

    // The audit stream tag — both the log labels and the body selector.
    const std::string* event = j.get("event");
    const std::string* logger = j.get("logger");
    const std::string* stream = j.get("stream");
    CHECK(event && *event == "audit");    // Loki label selector {event="audit"}
    CHECK(logger && *logger == "audit");
    CHECK(stream && *stream == "audit");  // body selector, mirrors the label

    // Required audit schema.
    const std::string* action = j.get("action");
    const std::string* outcome = j.get("outcome");
    CHECK(action && *action == "session_enter");
    CHECK(outcome && *outcome == "success");

    // The shared log backbone still present (rides the #165 pipeline).
    const std::string* realm = j.get("realm");
    const std::string* process = j.get("process");
    const std::string* level = j.get("level");
    const std::string* severity = j.get("severity");
    const std::string* message = j.get("message");
    const std::string* ts = j.get("timestamp_ms");
    CHECK(realm && process && level && severity && message && ts);
    CHECK(*message == "session_enter");   // message == action for readability
    CHECK(!j.key_is_string("timestamp_ms"));  // bare number

    // Typed schema fields.
    const std::string* acct = j.get("account_id");
    const std::string* corr = j.get("correlation_id");
    const std::string* target = j.get("target");
    const std::string* peer = j.get("peer");
    CHECK(acct && *acct == "42" && !j.key_is_string("account_id"));  // bare number
    CHECK(corr && *corr == "99887766" && !j.key_is_string("correlation_id"));
    CHECK(target && *target == "Ragnaros" && j.key_is_string("target"));
    CHECK(peer && *peer == "10.0.0.5:51000");
    return 0;
}

// -------------------------------------------------------------------------
// Test 2: login-FAILURE carries the reason + actor but NO secret material.
// This is the #92 no-secrets guarantee, asserted on the rendered bytes.
// -------------------------------------------------------------------------
int test_login_failure_no_secrets() {
    // The exact secret material a login flow handles and must NEVER audit-log:
    // the attempted password, the stored SRP verifier, and a session key.
    const std::string kPassword = "hunter2-correct-horse";
    const std::string kVerifier = "deadbeefcafef00dverifier0123456789abcdef";
    const std::string kSessionKey = "0011223344556677889900aabbccddeeff-key";

    // A call site builds a failure record the way authd does: actor + reason
    // ONLY. There is no schema field for a secret, and the call site passes none.
    audit::Record rec;
    rec.action = audit::Action::kLoginFailure;
    rec.outcome = audit::Outcome::kFailure;
    rec.account_id = 42;              // account was resolved, proof failed
    rec.reason = "bad_credentials";  // WHY, without the attempted secret
    rec.peer = "10.0.0.5:51000";

    std::string line = audit::render_json(rec);

    Json j;
    CHECK(parse_object(line, j));

    // The reason IS present and machine-readable.
    const std::string* reason = j.get("reason");
    CHECK(reason && *reason == "bad_credentials");
    const std::string* outcome = j.get("outcome");
    CHECK(outcome && *outcome == "failure");
    // Actor is present (account was resolved before the proof failed).
    const std::string* acct = j.get("account_id");
    CHECK(acct && *acct == "42");
    // A failure renders at warn severity (audit-dashboard signal).
    const std::string* severity = j.get("severity");
    const std::string* level = j.get("level");
    CHECK(severity && *severity == "warn");
    CHECK(level && *level == "warn");

    // The no-secrets guarantee: NONE of the secret material appears anywhere in
    // the rendered line — not as a value, not embedded in another field.
    CHECK(!has(line, kPassword));
    CHECK(!has(line, kVerifier));
    CHECK(!has(line, kSessionKey));
    // And no field named like a secret is present.
    CHECK(j.get("password") == nullptr);
    CHECK(j.get("verifier") == nullptr);
    CHECK(j.get("session_key") == nullptr);
    return 0;
}

// -------------------------------------------------------------------------
// Test 3: grant-REJECT carries the reject reason code + correlation id.
// -------------------------------------------------------------------------
int test_grant_reject_reason() {
    const std::string kSessionKey = "should-never-appear-session-key-bytes";

    audit::Record rec;
    rec.action = audit::Action::kGrantRejected;
    rec.outcome = audit::Outcome::kFailure;
    rec.reason = "grant_replay";       // machine-readable reject code
    rec.correlation_id = 123456789;    // the offending grant_id

    std::string line = audit::render_json(rec);

    Json j;
    CHECK(parse_object(line, j));
    const std::string* action = j.get("action");
    const std::string* reason = j.get("reason");
    const std::string* corr = j.get("correlation_id");
    CHECK(action && *action == "grant_rejected");
    CHECK(reason && *reason == "grant_replay");
    CHECK(corr && *corr == "123456789" && !j.key_is_string("correlation_id"));
    // A reject before the account is resolved has no actor.
    CHECK(j.get("account_id") == nullptr);
    // No secret leaks.
    CHECK(!has(line, kSessionKey));
    CHECK(j.get("session_key") == nullptr);
    return 0;
}

// -------------------------------------------------------------------------
// Test 4: optional fields omitted when empty/zero (compact records).
// -------------------------------------------------------------------------
int test_optional_omission() {
    audit::Record rec;
    rec.action = audit::Action::kGrantIssued;
    rec.outcome = audit::Outcome::kSuccess;
    rec.account_id = 7;
    rec.correlation_id = 555;
    // No target, no reason, no peer, no extra.

    std::string line = audit::render_json(rec);
    Json j;
    CHECK(parse_object(line, j));
    CHECK(j.get("target") == nullptr);   // empty -> omitted
    CHECK(j.get("reason") == nullptr);   // empty -> omitted
    CHECK(j.get("peer") == nullptr);     // empty -> omitted
    CHECK(j.get("account_id") != nullptr);
    CHECK(j.get("correlation_id") != nullptr);
    // A success renders at info severity.
    const std::string* severity = j.get("severity");
    CHECK(severity && *severity == "info");
    return 0;
}

// -------------------------------------------------------------------------
// Test 5: action + outcome vocabulary.
// -------------------------------------------------------------------------
int test_vocab() {
    CHECK(std::string(audit::action_name(audit::Action::kLoginSuccess)) == "login_success");
    CHECK(std::string(audit::action_name(audit::Action::kLoginFailure)) == "login_failure");
    CHECK(std::string(audit::action_name(audit::Action::kGrantIssued)) == "grant_issued");
    CHECK(std::string(audit::action_name(audit::Action::kGrantConsumed)) == "grant_consumed");
    CHECK(std::string(audit::action_name(audit::Action::kGrantRejected)) == "grant_rejected");
    CHECK(std::string(audit::action_name(audit::Action::kSessionEnter)) == "session_enter");
    CHECK(std::string(audit::action_name(audit::Action::kSessionLeave)) == "session_leave");
    CHECK(std::string(audit::outcome_name(audit::Outcome::kSuccess)) == "success");
    CHECK(std::string(audit::outcome_name(audit::Outcome::kFailure)) == "failure");
    CHECK(audit::level_for(audit::Outcome::kSuccess) == mlog::Level::Info);
    CHECK(audit::level_for(audit::Outcome::kFailure) == mlog::Level::Warn);
    return 0;
}

}  // namespace

int main() {
    if (test_audit_shape() != 0) return 1;
    if (test_login_failure_no_secrets() != 0) return 1;
    if (test_grant_reject_reason() != 0) return 1;
    if (test_optional_omission() != 0) return 1;
    if (test_vocab() != 0) return 1;
    std::printf("OK meridian-core audit: %d checks passed\n", g_checks);
    return 0;
}
