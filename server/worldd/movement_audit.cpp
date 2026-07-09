// SPDX-License-Identifier: Apache-2.0
//
// worldd — ANTI-CHEAT movement audit record builder (OPS-03a, #420).
// See movement_audit.h for the provenance + design.

#include "movement_audit.h"

#include <array>
#include <cstdio>
#include <string>

namespace meridian::worldd {
namespace audit = meridian::core::audit;
namespace log = meridian::core::log;

const char* move_reject_reason(MoveReject reject) {
    switch (reject) {
        case MoveReject::kNone:              return "none";
        case MoveReject::kSpeedPerPacket:    return "speed_per_packet";
        case MoveReject::kSpeedWindow:       return "speed_window";
        case MoveReject::kOutOfBounds:       return "out_of_bounds";
        case MoveReject::kZOutOfRange:       return "z_out_of_range";
        case MoveReject::kTeleport:          return "teleport";
        case MoveReject::kIllegalFlag:       return "illegal_flag";
        case MoveReject::kStaleSequence:     return "stale_sequence";
        case MoveReject::kUnackedForcedMove: return "unacked_forced_move";
    }
    return "none";
}

namespace {

// Format the snapped-back position as a compact, human- + machine-readable STRING
// (not bare floats) so the audit line stays a flat, integer/string/bool JSON object
// the shared minimal parser (log_test / audit_test) can validate. 2 decimals is
// ample forensic precision for a zone-local metre coordinate.
std::string format_pos(const Position& p) {
    std::array<char, 96> buf{};
    std::snprintf(buf.data(), buf.size(), "%.2f,%.2f,%.2f",
                  static_cast<double>(p.x), static_cast<double>(p.y),
                  static_cast<double>(p.z));
    return std::string(buf.data());
}

}  // namespace

audit::Record build_movement_reject_audit(std::uint64_t account_id,
                                          std::uint64_t grant_id,
                                          std::uint64_t entity_guid,
                                          MoveReject reject,
                                          const Position& snapped_back,
                                          std::uint32_t ack_seq) {
    audit::Record rec;
    rec.action = audit::Action::kMovementRejected;
    rec.outcome = audit::Outcome::kFailure;   // a violation is a warn-level flag
    rec.account_id = account_id;              // 0 => omitted
    rec.target = "entity:" + std::to_string(entity_guid);  // the offending mover
    rec.reason = move_reject_reason(reject);  // fine-grained cause
    rec.correlation_id = grant_id;            // 0 => omitted; pivots audit↔trace↔log
    // Forensic context: where the client was snapped back to + which intent tripped.
    rec.extra.push_back(log::field("snap_pos", format_pos(snapped_back)));
    rec.extra.push_back(log::field("ack_seq", static_cast<std::uint64_t>(ack_seq)));
    return rec;
}

}  // namespace meridian::worldd
