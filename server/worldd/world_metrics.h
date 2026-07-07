// SPDX-License-Identifier: Apache-2.0
//
// worldd — OPS-05 metric label context + helpers (#164; server SAD §8.5;
// docs/telemetry-architecture.md §5).
//
// The catalog's worldd metrics are map-scoped: {realm, zone, shard, ...} per
// D-23 (SAD §8.5 "re-labelled with {realm, zone, shard} where map-scoped"). This
// header carries that label context so every worldd instrumentation call site
// stamps the same {realm, zone, shard, map} — the exact labels the Grafana
// panels group by (server/ops/grafana/dashboards/{realm-health,player-experience,
// errors}.json). It also names opcodes/reasons for the low-cardinality label
// values the catalog uses (opcode, reason, kind).
//
// Clean-room from the SAD + the signal catalog; no GPL source consulted.

#ifndef MERIDIAN_WORLDD_WORLD_METRICS_H
#define MERIDIAN_WORLDD_WORLD_METRICS_H

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "meridian/net/tls_listener.h"  // net::Opcode, DisconnectReason
#include "movement_validation.h"        // MoveReject
#include "world_generated.h"            // EnumNameOpcode

namespace meridian::worldd {

// The map-scoped label context a worldd process stamps on its metrics. At M0
// (D-19 flat bootstrap map) zone/shard/map are single values; the fields exist so
// the labels are already correct when real zones/shards land (M1/M2) — no metric
// rename, matching the catalog's forward-compatible {realm,zone,shard,map}.
struct MetricsLabels {
    std::string realm = "reference";  // realm name (auth domain, D-23)
    std::string zone = "0";           // zone id (M0 bootstrap: 0)
    std::string shard = "0";          // shard index (M0: single shard 0)
    std::string map = "bootstrap";    // map name (D-19 flat bootstrap map)

    // Label tuples for the catalog families, in the declared label order.
    std::vector<std::string> rzs() const { return {realm, zone, shard}; }
    std::vector<std::string> rzsm() const { return {realm, zone, shard, map}; }
    std::vector<std::string> rzs_opcode(const std::string& op) const {
        return {realm, zone, shard, op};
    }
    std::vector<std::string> rzs_opcode_reason(const std::string& op,
                                               const std::string& reason) const {
        return {realm, zone, shard, op, reason};
    }
};

// The opcode label value: the world.fbs enum name (e.g. "WORLD_HELLO",
// "MOVEMENT_INTENT"), or a hex fallback for an unknown/reserved value — a stable,
// bounded set (the opcode registry is small + fixed), safe as a label.
inline std::string opcode_label(std::uint16_t op) {
    const char* n = net::EnumNameOpcode(static_cast<net::Opcode>(op));
    if (n && n[0] != '\0') return n;
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%04X", op);
    return std::string(buf);
}

// The DisconnectReason label value (world.fbs enum name), for
// meridian_disconnects_total{realm,reason}.
inline std::string disconnect_reason_label(net::DisconnectReason r) {
    const char* n = net::EnumNameDisconnectReason(r);
    return (n && n[0] != '\0') ? std::string(n) : std::string("UNKNOWN");
}

// The movement-violation `kind` label value (catalog
// meridian_movement_violations_total{...,kind}). The catalog names the taxonomy
// "speed/teleport/bounds/flag"; the M0 validator's MoveReject maps as: per-packet
// over-cap -> "speed", sliding-window over-cap (burst-then-idle, catches the
// teleport/blink cheat) -> "teleport", outside map bounds -> "bounds", z outside
// the ground envelope -> "z" (its own kind — an honest label rather than forcing
// it into "flag", which is reserved for state-flag violations landing with the v1
// envelope at M1). Low-cardinality, stable strings safe as a label.
inline std::string move_reject_kind(MoveReject r) {
    switch (r) {
        case MoveReject::kSpeedPerPacket: return "speed";
        case MoveReject::kSpeedWindow:    return "teleport";
        case MoveReject::kOutOfBounds:    return "bounds";
        case MoveReject::kZOutOfRange:    return "z";
        case MoveReject::kNone:           return "none";
    }
    return "none";
}

}  // namespace meridian::worldd

#endif  // MERIDIAN_WORLDD_WORLD_METRICS_H
