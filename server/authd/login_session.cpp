// SPDX-License-Identifier: Apache-2.0
//
// authd — IF-1 login state machine implementation (server SAD §5.1, §3.1;
// issue #79). See login_session.h for the provenance + clean-room statement.
//
// Flow (SAD §5.1, wire contract schema/net/auth.fbs):
//   ClientHello{build,proto_ver}  -> ServerHello{build,proto_ver} | Error
//   SrpStart{account}             -> SrpChallenge{salt,B}
//   SrpProof{A,M1}                -> AuthResult{m2} | AuthResult{error}
//   RealmListRequest              -> RealmList{realms[]}
//   RealmSelect{realm_id}         -> SessionGrant{grant_id,session_key,rc} | Error
//   then authd closes (no long-lived auth connections — SAD §5.1).
//
// Each inbound frame is a FlatBuffer root table; auth.fbs declares no root_type,
// so we decode with the generic flatbuffers::GetRoot<T>/VerifyBuffer<T> (as the
// proto round-trip proof does) — the message type at each step is fixed by the
// strictly-sequenced handshake, so there is no opcode to dispatch on.

#include "login_session.h"

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <array>
#include <chrono>
#include <cstring>
#include <ctime>

#include "auth_generated.h"
#include "meridian/bans/bans.h"
#include "meridian/core/audit.hpp"
#include "meridian/core/log.hpp"  // #510 realm-list wire-path instrumentation
#include "meridian/metrics/catalog.h"
#include "meridian/trace/session_flow.h"
#include "meridian/trace/span.h"
#include "meridian/trace/tracer.h"

namespace meridian::authd {
namespace {

namespace fb = flatbuffers;
namespace mn = meridian::net;

// Map a LoginOutcome to the `outcome` label value used by meridian_auth_results_
// total / meridian_auth_srp_duration_seconds (OPS-05 signal catalog). Stable,
// low-cardinality strings so the Grafana Player-experience / Errors panels can
// group the login funnel by result.
const char* outcome_label(LoginOutcome o) {
    switch (o) {
        case LoginOutcome::kGranted:         return "granted";
        case LoginOutcome::kRejectedHello:   return "rejected_hello";
        case LoginOutcome::kRejectedAuth:    return "rejected_auth";
        case LoginOutcome::kRejectedRealm:   return "rejected_realm";
        case LoginOutcome::kRejectedBanned:  return "rejected_banned";
        case LoginOutcome::kProtocolError:   return "protocol_error";
        case LoginOutcome::kTransportClosed: return "transport_closed";
    }
    return "unknown";
}

// RAII: on construction bump meridian_auth_attempts_total{realm}; on destruction
// record the final outcome (meridian_auth_results_total{realm,outcome}) and the
// server-side SRP handshake duration (meridian_auth_srp_duration_seconds{realm,
// outcome}). Reads the LoginResult by reference so it captures whatever outcome
// run_login ends on, at ANY of its early returns — no per-return metric calls.
class LoginMetricsScope {
public:
    LoginMetricsScope(const std::string& realm, const LoginResult& result)
        : realm_(realm), result_(result),
          start_(std::chrono::steady_clock::now()) {
        namespace cat = meridian::metrics::catalog;
        cat::auth_attempts_total().with({realm_}).inc();
    }
    ~LoginMetricsScope() {
        namespace cat = meridian::metrics::catalog;
        const double elapsed_s =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();
        const std::string outcome = outcome_label(result_.outcome);
        cat::auth_results_total().with({realm_, outcome}).inc();
        cat::auth_srp_duration_seconds().with({realm_, outcome}).observe(elapsed_s);
    }

private:
    std::string realm_;
    const LoginResult& result_;
    std::chrono::steady_clock::time_point start_;
};

// RAII: on construction stamp the span start (UNIX-ns); on destruction build the
// "authd.login" session-flow span from the final LoginResult and hand it to the
// exporter. This is the AUTH → GRANT leg of the session-flow trace (D-29 §9 rule
// 5; docs/telemetry-architecture.md §5.3). Like LoginMetricsScope it reads the
// result BY REFERENCE, so it captures whatever outcome run_login ends on at any
// early return — no per-return span calls. No-op when no exporter is configured.
class LoginTraceScope {
public:
    LoginTraceScope(meridian::trace::Exporter* exporter, const std::string& realm,
                    LoginResult& result)
        : exporter_(exporter), realm_(realm), result_(result),
          start_ns_(meridian::trace::now_unix_nano()) {}

    ~LoginTraceScope() {
        namespace tr = meridian::trace;
        if (exporter_ == nullptr || !exporter_->active()) return;

        tr::Span span;
        // On a GRANTED login, derive the span ids from the grant_id so the worldd
        // enter-world hop stitches into the SAME trace without a wire change (D-29
        // §9 rule 5). The authd span IS the parent worldd points at, so we stamp it
        // with the derived parent span_id. On a non-granted outcome there is no
        // grant to stitch on -> a fresh, self-contained root trace.
        if (result_.outcome == LoginOutcome::kGranted && result_.grant_id != 0) {
            tr::SpanContext ctx = tr::flow::trace_context_from_grant(result_.grant_id);
            span.trace_id = ctx.trace_id;
            span.span_id = ctx.span_id;  // == the parent worldd derives + points at
        } else {
            tr::fill_random(span.trace_id);
            tr::fill_random(span.span_id);
        }
        // parent_span_id stays all-zero: at M0 the authd login span is the root of
        // the session-flow trace (no upstream service before authd).
        span.name = tr::flow::kAuthdLogin;
        span.kind = tr::SpanKind::kServer;
        span.start_unix_nano = start_ns_;
        span.end_unix_nano = tr::now_unix_nano();

        span.set(tr::attr(tr::flow::kAttrRealm, realm_));
        span.set(tr::attr(tr::flow::kAttrOutcome, std::string(outcome_label(result_.outcome))));
        span.set(tr::attr(tr::flow::kAttrGrantIssued, result_.grant_id != 0));
        if (result_.account_id != 0) {
            span.set(tr::attr("meridian.account_id",
                              static_cast<std::int64_t>(result_.account_id)));
        }
        // Status: a granted login is Ok; any rejection/protocol/transport outcome
        // is Error (the failure point the trace makes visible), with the detail as
        // the status message.
        if (result_.outcome == LoginOutcome::kGranted) {
            span.set_status(tr::StatusCode::kOk);
        } else {
            span.set_status(tr::StatusCode::kError, result_.detail);
        }

        // Publish the span's ids for the caller's log↔trace correlation (#165).
        result_.trace_id = tr::to_hex(span.trace_id);
        result_.span_id = tr::to_hex(span.span_id);

        exporter_->export_span(std::move(span));
    }

private:
    meridian::trace::Exporter* exporter_;
    std::string realm_;
    LoginResult& result_;
    std::uint64_t start_ns_;
};

using net::Bytes;

// ---- FlatBuffer encode helpers ---------------------------------------------
// Each helper finishes a builder on one message table and returns its bytes as
// a net::Bytes ready for Session::write_frame (the u32 LE length is prepended by
// the transport layer, so we hand it the bare FlatBuffer root).

Bytes finish(fb::FlatBufferBuilder& b) {
    return Bytes(b.GetBufferPointer(), b.GetBufferPointer() + b.GetSize());
}

Bytes encode_server_hello(std::uint32_t build, std::uint16_t proto_ver) {
    fb::FlatBufferBuilder b;
    auto h = mn::CreateServerHello(b, build, proto_ver);
    b.Finish(h);
    return finish(b);
}

Bytes encode_error(mn::AuthErrorCode code, const char* message) {
    fb::FlatBufferBuilder b;
    auto msg = b.CreateString(message);
    auto e = mn::CreateError(b, code, msg);
    b.Finish(e);
    return finish(b);
}

Bytes encode_srp_challenge(const Bytes& salt, const Bytes& B) {
    fb::FlatBufferBuilder b;
    auto salt_v = b.CreateVector(salt);
    auto b_v = b.CreateVector(B);
    auto c = mn::CreateSrpChallenge(b, salt_v, b_v);
    b.Finish(c);
    return finish(b);
}

Bytes encode_auth_result_ok(const Bytes& M2) {
    fb::FlatBufferBuilder b;
    auto m2_v = b.CreateVector(M2);
    // success=true, m2 populated, no error (auth.fbs AuthResult contract).
    auto r = mn::CreateAuthResult(b, /*success=*/true, m2_v, /*error=*/0);
    b.Finish(r);
    return finish(b);
}

Bytes encode_auth_result_error(mn::AuthErrorCode code, const char* message) {
    fb::FlatBufferBuilder b;
    auto msg = b.CreateString(message);
    auto err = mn::CreateError(b, code, msg);
    // success=false, no m2, error populated.
    auto r = mn::CreateAuthResult(b, /*success=*/false, /*m2=*/0, err);
    b.Finish(r);
    return finish(b);
}

Bytes encode_session_grant(std::uint64_t grant_id, const Bytes& session_key,
                           std::uint32_t reconnect_window_ms) {
    fb::FlatBufferBuilder b;
    auto key_v = b.CreateVector(session_key);
    auto g = mn::CreateSessionGrant(b, grant_id, key_v, reconnect_window_ms);
    b.Finish(g);
    return finish(b);
}

// ---- FlatBuffer decode helpers ---------------------------------------------
// Verify then GetRoot; return nullptr on a malformed/oversized buffer so the
// caller can treat it as a protocol error (untrusted peer — never trust length
// or offsets without the Verifier).

template <typename T>
const T* decode(const Bytes& buf) {
    fb::Verifier v(buf.data(), buf.size());
    if (!v.VerifyBuffer<T>(nullptr)) return nullptr;
    return fb::GetRoot<T>(buf.data());
}

// Copy a FlatBuffer [ubyte] vector into a net::Bytes (empty if absent).
Bytes vec_to_bytes(const fb::Vector<std::uint8_t>* v) {
    if (v == nullptr) return {};
    return Bytes(v->data(), v->data() + v->size());
}

// ---- crypto / random helpers -----------------------------------------------

// 32 random bytes for a session_key (SAD §4.1 BINARY(32)). Throws on RNG
// failure — a login must never proceed with weak key material.
Bytes random_key32() {
    Bytes k(32);
    if (RAND_bytes(k.data(), static_cast<int>(k.size())) != 1) {
        throw net::TlsError("RAND_bytes failed for session_key");
    }
    return k;
}

// A random, non-zero u64 grant_id (SAD §4.1: random, NOT auto-increment, so it
// is unguessable / non-enumerable). Zero is avoided so callers can treat 0 as
// "no grant".
std::uint64_t random_grant_id() {
    std::uint64_t id = 0;
    do {
        if (RAND_bytes(reinterpret_cast<unsigned char*>(&id), sizeof(id)) != 1) {
            throw net::TlsError("RAND_bytes failed for grant_id");
        }
    } while (id == 0);
    return id;
}

// Anti-enumeration fake salt for an unknown username (SAD §2.1). Deterministic
// per {username, server secret} so a repeat SrpStart for the same missing user
// yields the SAME salt (a real account's salt is stable too — a varying salt
// would itself be an oracle). We derive SHA-256(secret || username) and take 32
// bytes. The secret is process-random: it need not persist (the goal is only to
// deny an attacker a "user exists?" signal within a run), and a fresh secret per
// process is strictly safer.
const std::array<std::uint8_t, 32>& enumeration_secret() {
    static const std::array<std::uint8_t, 32> secret = [] {
        std::array<std::uint8_t, 32> s{};
        if (RAND_bytes(s.data(), static_cast<int>(s.size())) != 1) {
            // Extremely unlikely; fall back to a fixed value rather than crash —
            // the anti-enumeration property degrades but login still works.
            s.fill(0x5A);
        }
        return s;
    }();
    return secret;
}

Bytes fake_salt_for(std::string_view username) {
    // SHA-256(secret || username) via the non-deprecated EVP one-shot digest.
    const auto& secret = enumeration_secret();
    Bytes input;
    input.reserve(secret.size() + username.size());
    input.insert(input.end(), secret.begin(), secret.end());
    input.insert(input.end(), username.begin(), username.end());

    Bytes salt(32);
    unsigned int out_len = 0;
    if (EVP_Digest(input.data(), input.size(), salt.data(), &out_len,
                   EVP_sha256(), nullptr) != 1 ||
        out_len != 32) {
        throw net::TlsError("EVP_Digest(SHA-256) failed for anti-enumeration salt");
    }
    return salt;
}

// Lowercase hex of raw bytes, for the #510/#518 INFO ground-truth logging of the
// decoded username (the username is NOT a secret). NEVER pass a password, SRP
// verifier, salt, session key, or M1/M2 through here.
std::string hex_encode(std::string_view bytes) {
    static const char* hexd = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (unsigned char byte : bytes) {
        out.push_back(hexd[byte >> 4]);
        out.push_back(hexd[byte & 0x0F]);
    }
    return out;
}

// Format a UTC DATETIME string "YYYY-MM-DD HH:MM:SS" `ttl` seconds from now, for
// the session_grant.expires_at column (SAD §4.1 — server writes UTC).
std::string utc_expires_in(std::uint32_t ttl_seconds) {
    std::time_t now = std::time(nullptr);
    std::time_t exp = now + static_cast<std::time_t>(ttl_seconds);
    std::tm tm_utc{};
    gmtime_r(&exp, &tm_utc);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_utc);
    return std::string(buf);
}

// Parse a decimal string cell to u64 (0 on absent/garbage — callers validate).
std::uint64_t cell_u64(const db::Cell& c) {
    if (!c.has_value()) return 0;
    return std::strtoull(c->c_str(), nullptr, 10);
}

// Bind a Bytes as a DB blob param.
db::Param blob(const Bytes& b) {
    return db::Param{db::Bytes_t(b.begin(), b.end())};
}

}  // namespace

LoginResult run_login(net::Session& sess, db::Connection& db,
                      const LoginConfig& cfg) {
    LoginResult result;

    // OPS-05: count this attempt now and (on scope exit, at whichever return the
    // flow ends on) record the outcome + server-side SRP handshake duration
    // (meridian_auth_{attempts,results}_total + meridian_auth_srp_duration_seconds).
    LoginMetricsScope metrics_scope(cfg.realm_label, result);

    // OPS-05 traces (#166): on scope exit, emit the "authd.login" session-flow span
    // (SRP verify → grant issue) to the OTLP exporter, if one is configured. No-op
    // otherwise — the login flow itself is untouched (graceful degradation).
    LoginTraceScope trace_scope(cfg.tracer_exporter, cfg.realm_label, result);

    // ---- 1. ClientHello -> ServerHello | Error ------------------------------
    Bytes frame;
    try {
        frame = sess.read_frame();
    } catch (const net::ConnectionClosed&) {
        result.outcome = LoginOutcome::kTransportClosed;
        result.detail = "peer closed before ClientHello";
        return result;
    }

    const mn::ClientHello* hello = decode<mn::ClientHello>(frame);
    if (hello == nullptr) {
        sess.write_frame(encode_error(mn::AuthErrorCode::INTERNAL,
                                      "malformed ClientHello"));
        result.outcome = LoginOutcome::kProtocolError;
        result.detail = "undecodable ClientHello";
        return result;
    }

    const std::uint32_t client_build = hello->build();
    if (hello->proto_ver() != cfg.proto_ver) {
        sess.write_frame(encode_error(mn::AuthErrorCode::PROTOCOL_MISMATCH,
                                      "unsupported protocol version"));
        result.outcome = LoginOutcome::kRejectedHello;
        result.detail = "proto_ver mismatch";
        return result;
    }
    if (cfg.build_floor != 0 && client_build < cfg.build_floor) {
        sess.write_frame(encode_error(mn::AuthErrorCode::BUILD_REJECTED,
                                      "client build below floor"));
        result.outcome = LoginOutcome::kRejectedHello;
        result.detail = "build below floor";
        return result;
    }

    // OPS-02c (#419): refuse a banned SOURCE IP before any SRP work — an IP ban is
    // account-independent, so it gates the earliest point we have a validated peer.
    // The check is exact-match on the peer's address (port stripped). A DB error
    // here must NOT wedge login (fail-open on the moderation lookup); the account
    // ban below still gates the credentialed path.
    const std::string peer_ip = bans::ip_of_peer(sess.peer());
    try {
        if (!peer_ip.empty() && bans::ip_ban(db, peer_ip).has_value()) {
            sess.write_frame(encode_error(mn::AuthErrorCode::ACCOUNT_BANNED,
                                          "this address is banned"));
            core::audit::emit(core::audit::Record{
                .action = core::audit::Action::kBanRejected,
                .outcome = core::audit::Outcome::kFailure,
                .target = "ip:" + peer_ip,
                .reason = "ip_banned",
                .peer = sess.peer(),
            });
            result.outcome = LoginOutcome::kRejectedBanned;
            result.detail = "source IP banned";
            return result;
        }
    } catch (const db::DbError& e) {
        // Moderation lookup fault: log-and-continue (do not deny a legitimate login
        // on a transient DB hiccup); the account-ban check remains the gate.
    }

    sess.write_frame(encode_server_hello(cfg.server_build, cfg.proto_ver));

    // ---- 2. SrpStart{account} -> SrpChallenge{salt,B} -----------------------
    frame = sess.read_frame();
    const mn::SrpStart* start = decode<mn::SrpStart>(frame);
    if (start == nullptr || start->account() == nullptr) {
        sess.write_frame(encode_error(mn::AuthErrorCode::INTERNAL,
                                      "malformed SrpStart"));
        result.outcome = LoginOutcome::kProtocolError;
        result.detail = "undecodable SrpStart";
        return result;
    }
    const std::string username = start->account()->str();

    // #510/#518 ground truth: log the decoded username bytes at INFO so a live
    // FAILING session reveals whether the wire delivered a corrupt/empty username
    // (framing bug) or a valid one. username_hex is the raw bytes — the username is
    // NOT a secret; the password/verifier/salt/session key/M1/M2 are never logged.
    core::log::info(
        "authd", "login username decoded",
        {core::log::field("peer", sess.peer()),
         core::log::field("username_len",
                          static_cast<std::int64_t>(username.size())),
         core::log::field("username_hex", hex_encode(username))});

    // Load {salt, verifier} for the account (SAD §5.1 "load {salt, verifier}").
    // Parameterized — the username binds through a prepared-statement parameter,
    // never concatenated (backend standards; meridian-db contract).
    db::Result acct = db.execute(
        "SELECT id, srp_salt, srp_verifier, gm_level FROM account WHERE username = ?",
        {db::Param{username}});

    Bytes salt, verifier;
    std::uint64_t account_id = 0;
    std::uint8_t gm_level = 0;  // account's GM tier (D-16; #417) — exposed via result
    bool known_user = (acct.rows.size() == 1);

    // #510/#518 ground truth: log the account-lookup outcome at INFO. Pins whether
    // a FAILING session genuinely got 0 rows for a valid username (DB mystery) vs a
    // wire-corrupt username that simply doesn't exist. Correlate with the decode
    // line above via peer + username_len.
    core::log::info(
        "authd", "account lookup",
        {core::log::field("peer", sess.peer()),
         core::log::field("username_len",
                          static_cast<std::int64_t>(username.size())),
         core::log::field("account_rows",
                          static_cast<std::int64_t>(acct.rows.size())),
         core::log::field("account_found", known_user)});
    if (known_user) {
        const db::Row& r = acct.rows[0];
        account_id = cell_u64(r[0]);
        if (r[1].has_value()) salt.assign(r[1]->begin(), r[1]->end());
        if (r[2].has_value()) verifier.assign(r[2]->begin(), r[2]->end());
        gm_level = static_cast<std::uint8_t>(cell_u64(r[3]));
    } else {
        // Anti-enumeration: fabricate a stable fake salt + a throwaway verifier
        // so an unknown user is indistinguishable from a wrong password. We
        // derive a verifier from a random password over the fake salt — the
        // client can never produce a matching M1, so verify() always rejects.
        salt = fake_salt_for(username);
        Bytes fake_pw = random_key32();
        std::string pw_hex;
        pw_hex.reserve(fake_pw.size() * 2);
        static const char* hexd = "0123456789abcdef";
        for (std::uint8_t byte : fake_pw) {
            pw_hex.push_back(hexd[byte >> 4]);
            pw_hex.push_back(hexd[byte & 0x0F]);
        }
        srp::Verifier v =
            srp::make_verifier(username, pw_hex, cfg.srp_params, salt);
        verifier = v.verifier;
    }
    result.account_id = account_id;
    result.gm_level = gm_level;  // 0 for an unknown user (anti-enumeration path)

    srp::ServerSession srp(username, salt, verifier, cfg.srp_params);
    sess.write_frame(encode_srp_challenge(salt, srp.B()));

    // ---- 3. SrpProof{A,M1} -> AuthResult{m2} | AuthResult{error} ------------
    frame = sess.read_frame();
    const mn::SrpProof* proof = decode<mn::SrpProof>(frame);
    if (proof == nullptr || proof->a() == nullptr || proof->m1() == nullptr) {
        sess.write_frame(encode_auth_result_error(mn::AuthErrorCode::INTERNAL,
                                                  "malformed SrpProof"));
        result.outcome = LoginOutcome::kProtocolError;
        result.detail = "undecodable SrpProof";
        return result;
    }
    Bytes A = vec_to_bytes(proof->a());
    Bytes M1 = vec_to_bytes(proof->m1());

    std::optional<Bytes> M2 = srp.verify(A, M1);
    if (!M2.has_value() || !known_user) {
        // Wrong proof, or an unknown user (whose throwaway verifier can never
        // match): same BAD_CREDENTIALS outcome, no grant written (SAD §5.1).
        sess.write_frame(encode_auth_result_error(
            mn::AuthErrorCode::BAD_CREDENTIALS, "authentication failed"));
        result.outcome = LoginOutcome::kRejectedAuth;
        result.detail = known_user ? "bad SRP proof" : "unknown user";
        return result;
    }

    // OPS-02c (#419): the credentials verified — now refuse a BANNED account. This
    // runs AFTER the SRP proof (the actor is proven, so surfacing "banned" leaks
    // nothing they don't already know) and BEFORE any grant is written, so a banned
    // account never receives a SessionGrant. A moderation-lookup DB error fails
    // OPEN (a transient hiccup must not lock out a legitimate, non-banned login).
    try {
        if (std::optional<bans::Active> ban = bans::account_ban(db, account_id)) {
            sess.write_frame(encode_auth_result_error(
                mn::AuthErrorCode::ACCOUNT_BANNED, "this account is banned"));
            core::audit::emit(core::audit::Record{
                .action = core::audit::Action::kBanRejected,
                .outcome = core::audit::Outcome::kFailure,
                .account_id = account_id,
                .target = "account:" + std::to_string(account_id),
                .reason = "account_banned",
                .peer = sess.peer(),
            });
            result.outcome = LoginOutcome::kRejectedBanned;
            result.detail = "account banned";
            return result;
        }
    } catch (const db::DbError& e) {
        // Fail-open on a moderation lookup fault (see the IP-ban note above).
    }

    sess.write_frame(encode_auth_result_ok(*M2));

    // ---- 4. RealmListRequest -> RealmList -----------------------------------
    frame = sess.read_frame();
    const mn::RealmListRequest* rlr = decode<mn::RealmListRequest>(frame);
    if (rlr == nullptr) {
        // #510 observability: this path was effectively silent (only surfaced as a
        // generic kProtocolError detail on the main login log). Log it explicitly,
        // WITH the received byte length, so a live-network framing/misalignment bug
        // that corrupts the RealmListRequest is diagnosable from the authd logs —
        // the client sees only "empty realm list" and cannot tell us this happened.
        core::log::warn(
            "authd", "malformed RealmListRequest — sending Error(INTERNAL)",
            {core::log::field("account_id",
                              static_cast<std::int64_t>(account_id)),
             core::log::field("peer", sess.peer()),
             core::log::field("frame_bytes",
                              static_cast<std::int64_t>(frame.size()))});
        sess.write_frame(encode_error(mn::AuthErrorCode::INTERNAL,
                                      "malformed RealmListRequest"));
        result.outcome = LoginOutcome::kProtocolError;
        result.detail = "undecodable RealmListRequest";
        return result;
    }

    // Serve the realm table (SAD §4.1). Build the RealmList FlatBuffer directly
    // from the DB rows.
    db::Result realms = db.execute(
        "SELECT id, name, address, port, population, build_min, build_max, "
        "flags FROM realm ORDER BY id");
    {
        fb::FlatBufferBuilder b;
        std::vector<fb::Offset<mn::RealmRow>> rows;
        rows.reserve(realms.rows.size());
        for (const db::Row& r : realms.rows) {
            auto name = b.CreateString(r[1].has_value() ? *r[1] : "");
            auto addr = b.CreateString(r[2].has_value() ? *r[2] : "");
            rows.push_back(mn::CreateRealmRow(
                b,
                static_cast<std::uint32_t>(cell_u64(r[0])),
                name, addr,
                static_cast<std::uint16_t>(cell_u64(r[3])),
                static_cast<std::uint16_t>(cell_u64(r[4])),  // population bucket
                static_cast<std::uint32_t>(cell_u64(r[5])),
                static_cast<std::uint32_t>(cell_u64(r[6])),
                static_cast<std::uint32_t>(cell_u64(r[7]))));
        }
        auto vec = b.CreateVector(rows);
        auto list = mn::CreateRealmList(b, vec);
        b.Finish(list);
        Bytes realm_list_frame = finish(b);
        // #510 instrumentation: the DB row count served AND the RealmList frame
        // byte length sent. The live symptom is a client "empty realm list" while
        // the realm table has a row — this line pins whether authd read the row and
        // how many bytes it put on the wire, distinguishing candidate (a) "authd
        // sent 0 rows" from (b) "client mis-read a non-empty RealmList".
        core::log::debug(
            "authd", "serving RealmList",
            {core::log::field("account_id",
                              static_cast<std::int64_t>(account_id)),
             core::log::field("realm_rows",
                              static_cast<std::int64_t>(realms.rows.size())),
             core::log::field("realmlist_frame_bytes",
                              static_cast<std::int64_t>(realm_list_frame.size()))});
        // #510/#518 ground truth at INFO: the runtime log level suppresses DEBUG (and
        // ArgoCD reverts env-toggled levels), so the realm row count served must also
        // land at INFO to be captured on the live realm. The client sees only "empty
        // realm list" — this line pins whether authd actually served 0 rows.
        core::log::info(
            "authd", "realm list served",
            {core::log::field("peer", sess.peer()),
             core::log::field("realm_rows",
                              static_cast<std::int64_t>(realms.rows.size()))});
        sess.write_frame(realm_list_frame);
    }

    // ---- 5. RealmSelect{realm_id} -> SessionGrant | Error -------------------
    frame = sess.read_frame();
    const mn::RealmSelect* select = decode<mn::RealmSelect>(frame);
    if (select == nullptr) {
        sess.write_frame(encode_error(mn::AuthErrorCode::INTERNAL,
                                      "malformed RealmSelect"));
        result.outcome = LoginOutcome::kProtocolError;
        result.detail = "undecodable RealmSelect";
        return result;
    }
    const std::uint32_t realm_id = select->realm_id();

    // Validate the realm exists and accepts this client_build (SAD §5.1
    // "REALM_UNAVAILABLE / build range").
    db::Result realm = db.execute(
        "SELECT build_min, build_max FROM realm WHERE id = ?",
        {db::Param{static_cast<std::int64_t>(realm_id)}});
    if (realm.rows.size() != 1) {
        sess.write_frame(encode_error(mn::AuthErrorCode::REALM_UNAVAILABLE,
                                      "no such realm"));
        result.outcome = LoginOutcome::kRejectedRealm;
        result.detail = "unknown realm_id";
        return result;
    }
    {
        const db::Row& r = realm.rows[0];
        std::uint32_t bmin = static_cast<std::uint32_t>(cell_u64(r[0]));
        std::uint32_t bmax = static_cast<std::uint32_t>(cell_u64(r[1]));
        if (client_build < bmin || (bmax != 0 && client_build > bmax)) {
            sess.write_frame(encode_error(mn::AuthErrorCode::BUILD_REJECTED,
                                          "client build outside realm range"));
            result.outcome = LoginOutcome::kRejectedRealm;
            result.detail = "build outside realm range";
            return result;
        }
    }

    // Write the single-use session_grant (SAD §3.1, §4.1): random u64 grant_id,
    // 32-byte random session_key, expires_at = now + ttl, consumed_at NULL,
    // bound to {account, realm, client_build}. Parameterized INSERT.
    std::uint64_t grant_id = random_grant_id();
    Bytes session_key = random_key32();
    std::string expires_at = utc_expires_in(cfg.grant_ttl_seconds);

    // grant_id is a full-range random u64 (SAD §4.1). meridian-db binds an
    // int64 as a SIGNED LONGLONG, which MariaDB rejects ("out of range") for a
    // BIGINT UNSIGNED column once the value exceeds INT64_MAX. Bind it (and
    // account_id, also BIGINT UNSIGNED) as a DECIMAL STRING instead: MariaDB
    // parses the string into the column's unsigned range losslessly, and the
    // prepared statement keeps it injection-proof (it is a bound value, not
    // concatenated SQL).
    db.execute(
        "INSERT INTO session_grant "
        "(grant_id, account_id, realm_id, session_key, client_build, "
        " expires_at) VALUES (?, ?, ?, ?, ?, ?)",
        {db::Param{std::to_string(grant_id)},
         db::Param{std::to_string(account_id)},
         db::Param{static_cast<std::int64_t>(realm_id)},
         blob(session_key),
         db::Param{static_cast<std::int64_t>(client_build)},
         db::Param{expires_at}});

    sess.write_frame(
        encode_session_grant(grant_id, session_key, cfg.reconnect_window_ms));

    // authd closes: no long-lived auth connections (SAD §5.1).
    result.outcome = LoginOutcome::kGranted;
    result.grant_id = grant_id;
    result.detail = "grant issued";
    return result;
}

}  // namespace meridian::authd
