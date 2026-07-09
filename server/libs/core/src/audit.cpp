// SPDX-License-Identifier: Apache-2.0
//
// libmeridian-core — structured AUDIT stream implementation (OPS-05, #92).
//
// See meridian/core/audit.hpp for the design. This file only assembles the typed
// Record into the ordered Fields body and hands it to the log pipeline
// (meridian/core/log.hpp) — it introduces no new sink, no new dependency, and no
// secret-bearing surface. Clean-room per CONTRIBUTING.md.

#include "meridian/core/audit.hpp"

namespace meridian::core::audit {

const char* outcome_name(Outcome outcome) {
    switch (outcome) {
        case Outcome::kSuccess: return "success";
        case Outcome::kFailure: return "failure";
    }
    return "success";
}

const char* action_name(Action action) {
    switch (action) {
        case Action::kLoginSuccess:  return "login_success";
        case Action::kLoginFailure:  return "login_failure";
        case Action::kGrantIssued:   return "grant_issued";
        case Action::kGrantConsumed: return "grant_consumed";
        case Action::kGrantRejected: return "grant_rejected";
        case Action::kSessionEnter:  return "session_enter";
        case Action::kSessionLeave:  return "session_leave";
        case Action::kMovementRejected: return "movement_rejected";
        case Action::kGmCommand:        return "gm_command";
        case Action::kRateLimited:      return "rate_limited";
        case Action::kEconomyRejected:  return "economy_rejected";
    }
    return "unknown";
}

log::Level level_for(Outcome outcome) {
    // A denied/failed security event is a Warn (it is the interesting signal on
    // the audit dashboard); a successful one is an Info. Both bypass the level
    // filter via write_always, so this only classifies severity — it never gates
    // emission.
    return outcome == Outcome::kFailure ? log::Level::Warn : log::Level::Info;
}

log::Fields build_fields(const Record& rec) {
    log::Fields fields;
    fields.reserve(8 + rec.extra.size());

    // The dedicated selector first, so every audit line is unambiguously tagged
    // in the body even before the parsed `event`/`logger` labels.
    fields.push_back(log::field("stream", std::string(kStream)));
    fields.push_back(log::field("action", std::string(action_name(rec.action))));
    fields.push_back(log::field("outcome", std::string(outcome_name(rec.outcome))));

    // Only populated optionals are recorded (compact records; an omitted actor
    // reads as "unauthenticated / not yet resolved").
    if (rec.account_id != 0) {
        fields.push_back(
            log::field("account_id", static_cast<std::int64_t>(rec.account_id)));
    }
    if (!rec.target.empty()) {
        fields.push_back(log::field("target", rec.target));
    }
    if (!rec.reason.empty()) {
        fields.push_back(log::field("reason", rec.reason));
    }
    if (rec.correlation_id != 0) {
        fields.push_back(log::field("correlation_id",
                                    static_cast<std::uint64_t>(rec.correlation_id)));
    }
    if (!rec.peer.empty()) {
        fields.push_back(log::field("peer", rec.peer));
    }
    for (const log::Field& f : rec.extra) {
        fields.push_back(f);
    }
    return fields;
}

std::string render_json(const Record& rec) {
    // The `message` is the action name so text-mode + a bare eyeball read stay
    // legible; the machine-readable copy lives in the `action` field.
    return log::render_json(level_for(rec.outcome), kCategory,
                            action_name(rec.action), build_fields(rec));
}

void emit(const Record& rec) {
    // write_always: unconditional (audit records survive the operational log
    // level). category = kCategory => JSON event/logger = "audit".
    log::write_always(level_for(rec.outcome), kCategory,
                      action_name(rec.action), build_fields(rec));
}

}  // namespace meridian::core::audit
