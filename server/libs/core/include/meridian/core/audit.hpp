// SPDX-License-Identifier: Apache-2.0
//
// libmeridian-core — structured AUDIT stream (OPS-05, #92).
//
// Clean-room: designed from the server PRD §6/OPS-05 ("separate append-only
// audit streams for GM actions, economy transactions, and anti-cheat flags"),
// server SAD §2.1/§5.1 (auth outcomes are audit-logged; "passwords never
// leaked"), §5.3 (single-use session grants), and docs/telemetry-privacy.md
// (#172 retention policy). No GPL source consulted. See CONTRIBUTING.md.
//
// WHAT THIS IS. A thin, typed facility for emitting security/compliance-relevant
// events as a DISTINCT, filterable stream on top of the existing OPS-05
// structured-log pipeline (#165, meridian/core/log.hpp). It is NOT a second sink:
// an audit record is a normal JSON log line on the SAME stdout the log collector
// already tails, so it flows to Loki (#165) unchanged. What makes it separable is
// the tagging:
//
//   * the log `event`/`logger` labels are set to "audit" (log::write's category),
//     so a Loki label selector `{event="audit"}` picks the audit stream, and
//   * a body field `stream="audit"` mirrors that selector for backends that query
//     the parsed body rather than the label.
//
// A record renders (via log.hpp) as, e.g.:
//   {"realm":"reference","process":"authd","level":"warn","event":"audit",
//    "severity":"warn","logger":"audit","message":"login_failure",
//    "timestamp_ms":N,"stream":"audit","action":"login_failure",
//    "outcome":"failure","account_id":42,"reason":"bad_credentials",
//    "correlation_id":0,"peer":"10.0.0.5:51000"}
//
// UNCONDITIONAL. Audit records are emitted via log::write_always — they bypass
// the operational MERIDIAN_LOG_LEVEL so raising the log level to quiet noise can
// never silently drop the audit trail.
//
// NO SECRETS (#92, SAD §2.1). An audit record carries identity + outcome ONLY.
// There is deliberately NO field for a password, SRP verifier, session key, or
// token, and callers MUST NOT stuff secret material into `reason`/`target`/
// `extra`. The Record schema below is the whole surface: an actor (account id), a
// named action, an outcome, an optional reason/target, and a correlation id.
//
// RETENTION (#172). docs/telemetry-privacy.md §4 sets the OPERATIONAL log store
// (Loki) at a 30-day default. Audit records ride the same pipeline but are a
// compliance artefact, not operational noise: an operator SHOULD route the
// `{event="audit"}` stream to a longer-lived, append-only store than the 30-day
// operational logs (dispute/abuse investigations outlive a single milestone's
// tuning window). The stream tag exists precisely to make that routing a
// one-selector rule. The concrete audit window is the owner's call alongside the
// #172 judgment-calls; this facility only guarantees the events are emitted and
// selectable. No PII beyond the account id + network peer is recorded, consistent
// with telemetry-privacy.md §3.

#ifndef MERIDIAN_CORE_AUDIT_HPP
#define MERIDIAN_CORE_AUDIT_HPP

#include <cstdint>
#include <string>
#include <string_view>

#include "meridian/core/log.hpp"

namespace meridian::core::audit {

// The log category (=> JSON `event` + `logger` labels) every audit record uses,
// and the body `stream` value that mirrors it. A Loki selector `{event="audit"}`
// or a body filter `stream="audit"` isolates the audit stream from operational
// logs on the shared sink.
inline constexpr std::string_view kCategory = "audit";
inline constexpr std::string_view kStream = "audit";

// The outcome of an audited action. Kept binary on purpose — a security event
// either succeeded or was denied; the WHY lives in Record::reason.
enum class Outcome {
    kSuccess,
    kFailure,
};

// "success" | "failure" — the JSON `outcome` value.
const char* outcome_name(Outcome outcome);

// A canonical, stable set of audit action names. Passing the enum (rather than a
// raw string) keeps the vocabulary closed and greppable; the taxonomy started as
// the M0 core set scoped by #92 — auth + grant + session lifecycle — and grows one
// action at a time as PRD §6 streams land: the ANTI-CHEAT movement flag
// (kMovementRejected, OPS-03a #420) is the first. GM-command + economy actions slot
// in beside these as those features land.
enum class Action {
    kLoginSuccess,     // authd: SRP proof verified, account authenticated
    kLoginFailure,     // authd: login rejected (build gate / bad creds / realm)
    kGrantIssued,      // authd: a single-use session grant was written for a login
    kGrantConsumed,    // worldd: a grant was validated + atomically consumed
    kGrantRejected,    // worldd: a WorldHello grant was rejected (replay/expiry/...)
    kSessionEnter,     // worldd: an authenticated session entered the world
    kSessionLeave,     // worldd: a session left the world / disconnected
    kMovementRejected, // worldd: an OPS-03 movement-validation violation (anti-cheat,
                       //         #420) — snap-back correction + audit flag. `reason`
                       //         is the reject kind (speed/teleport/bounds/flag/...).
    kGmCommand,        // worldd: a GM command attempt (OPS-02a, #417) — ALLOWED or
                       //         DENIED. `target` is the command, `reason` the denial
                       //         classification (insufficient_level/unknown_command),
                       //         and `extra` carries the actor's gm_level + args. Both
                       //         outcomes are recorded on the append-only GM audit
                       //         stream (PRD §6).
    kRateLimited,      // worldd: a per-opcode RATE-CLASS flood was dropped (OPS-03b,
                       //         #421) — the dispatcher's per-session rate gate refused
                       //         a frame that exceeded its opcode class ceiling.
                       //         `target` is the offending opcode, `reason` the rate
                       //         class (chat/move/action/session). Anti-cheat throughput
                       //         signal on the append-only audit stream (PRD §6).
    kEconomyRejected,  // worldd: a defense-in-depth ECONOMY sanity check rejected a
                       //         transaction with an impossible delta (OPS-03b, #421) —
                       //         a bad quantity, a negative price/credit, or a copper
                       //         over/underflow. `target` is the transaction path
                       //         (vendor_buy/vendor_sell/quest_reward/loot_money), the
                       //         `reason` the classification. The economy audit stream
                       //         (PRD §6), complementing the per-feature validation.
};

// The stable JSON `action` string for an Action (e.g. "login_failure"). This is
// the value dashboards + alerts key on; it never changes for a given Action.
const char* action_name(Action action);

// One audit record — the complete, typed schema. Only the populated optional
// fields are rendered (an empty string / zero id is omitted), so a record stays
// compact. NOTHING here is a secret; see the file header.
struct Record {
    Action action;                     // required: what happened
    Outcome outcome = Outcome::kSuccess;

    // The actor: the account the action is attributed to. 0 => unauthenticated /
    // not-yet-resolved (e.g. a login failure before the account is known) and is
    // omitted from the record. This is the ONLY identity recorded — no username,
    // email, or other PII (telemetry-privacy.md §3).
    std::uint64_t account_id = 0;

    // Optional object the action acted on (e.g. a character name on session
    // enter, a realm id). Empty => omitted. MUST NOT carry secret material.
    std::string target;

    // Optional machine-readable reason/classification for a failure or reject
    // (e.g. "bad_credentials", "grant_replay"). Empty => omitted. This is where a
    // login FAILURE records WHY without ever recording the attempted secret.
    std::string reason;

    // Optional correlation id tying this record to a session flow — the grant_id,
    // which is the same value the OPS-05 trace derives its ids from (#166), so one
    // id pivots across audit ↔ trace ↔ operational log. 0 => omitted.
    std::uint64_t correlation_id = 0;

    // Optional network peer ("ip:port") for the actor, where the call site has it
    // (an authd/worldd connection). Empty => omitted. An address, not an identity
    // — retained only for abuse investigation, consistent with §3.
    std::string peer;

    // Optional extra typed context, appended verbatim after the schema fields.
    // For rare call-site-specific detail; the standard fields above cover the
    // core set. MUST NOT carry secret material.
    log::Fields extra;
};

// Emit `rec` onto the shared OPS-05 structured-log sink (#165) as the audit
// stream. Renders through log::write_always (unconditional — see header) at
// Level::Info for a success and Level::Warn for a failure, with category
// kCategory so the record carries the audit labels. Thread-safe (the underlying
// log write is). NEVER pass secrets in any field.
void emit(const Record& rec);

// --- Testing seam -----------------------------------------------------------
// Render `rec` to its JSON line WITHOUT touching the sink or the log write lock
// (mirrors log::render_json). Used by the unit test to parse a record back and
// assert its schema + the no-secrets guarantee deterministically. Always JSON,
// regardless of the global log format. Not part of the hot path.
std::string render_json(const Record& rec);

// The log level an audit record renders at, given its outcome (Info for success,
// Warn for failure). Exposed for the testing seam / callers that want the level.
log::Level level_for(Outcome outcome);

// Build the ordered Fields body for `rec` (stream, action, outcome, then the
// populated optionals, then extra). Shared by emit() and render_json() so the
// on-sink record and the rendered testing-seam record are byte-identical.
log::Fields build_fields(const Record& rec);

}  // namespace meridian::core::audit

#endif  // MERIDIAN_CORE_AUDIT_HPP
