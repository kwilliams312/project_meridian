// SPDX-License-Identifier: Apache-2.0
//
// worldd — ANTI-CHEAT movement audit record (OPS-03a, #420; epic #21).
//
// CLEAN-ROOM: designed from docs/prd/server-prd.md §6 ("separate append-only audit
// streams for … anti-cheat flags"), docs/sad/server-sad.md §5.5 (the movement
// validation envelope + policy ladder "correct (snap-back) → … → anti-cheat audit
// flag for GM review"), and the OPS-05 audit facility (meridian/core/audit.hpp,
// #92). No GPL source consulted. See CONTRIBUTING.md.
//
// WHAT THIS IS. A thin, PURE builder that turns a rejected MovementIntent into the
// typed anti-cheat audit Record the dispatch emits onto the append-only OPS-05
// audit stream (`{event="audit"}`, action="movement_rejected"). Keeping it a pure
// function of (account, grant, entity, reject kind, snapped-back position, seq) —
// no socket, no clock, no DB — lets a unit test assert the EXACT record the live
// dispatch builds without capturing stdout or standing up a session.
//
// NO SECRETS (#92). Like every audit record, this carries identity + outcome +
// forensic context ONLY — the account/grant correlation, the offending entity, the
// authoritative (snapped-back) position and the intent seq. Never a session key.

#ifndef MERIDIAN_WORLDD_MOVEMENT_AUDIT_H
#define MERIDIAN_WORLDD_MOVEMENT_AUDIT_H

#include <cstdint>

#include "meridian/core/audit.hpp"
#include "movement_validation.h"  // MoveReject, Position

namespace meridian::worldd {

// The machine-readable `reason` string for a movement reject — the audit-stream
// classification a GM query / anomaly rule keys on. Stable, low-cardinality,
// distinct from the metrics `kind` label (world_metrics.h move_reject_kind): the
// audit reason is the FINE-GRAINED cause, the metric kind the coarse taxonomy.
const char* move_reject_reason(MoveReject reject);

// Build the append-only ANTI-CHEAT audit record for one rejected movement intent
// (server PRD §6). `account_id`/`grant_id` attribute + correlate the flag to the
// session (0 => omitted); `entity_guid` names the offending mover; `snapped_back`
// is the authoritative position the client is corrected to; `ack_seq` is the
// rejected intent's seq. The dispatch emits the result via core::audit::emit.
core::audit::Record build_movement_reject_audit(std::uint64_t account_id,
                                                std::uint64_t grant_id,
                                                std::uint64_t entity_guid,
                                                MoveReject reject,
                                                const Position& snapped_back,
                                                std::uint32_t ack_seq);

}  // namespace meridian::worldd

#endif  // MERIDIAN_WORLDD_MOVEMENT_AUDIT_H
