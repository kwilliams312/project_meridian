// SPDX-License-Identifier: Apache-2.0
//
// libmeridian-metrics — the Meridian signal catalog, as typed accessors.
//
// This is the ONE place the SAD §8.5 / telemetry-architecture.md §5 metric
// NAMES, TYPES and LABELS are declared for the C++ server. Instrumenting through
// these helpers (instead of literal strings scattered across the daemons)
// guarantees every emitted series matches the catalog — and therefore the
// Grafana dashboards in server/ops/grafana/dashboards/*.json, which query these
// exact names/labels (that is the OPS-05 contract: the daemons must emit what the
// already-built dashboards read).
//
// SOURCES (verbatim from docs/telemetry-architecture.md §5.1, cross-checked
// against the dashboard PromQL in server/ops/grafana/dashboards/*.json):
//   meridian_tick_duration_seconds  histogram {realm,zone,shard,map}
//   meridian_ccu                    gauge     {realm,zone,shard}
//   meridian_sessions               gauge     {realm,state}
//   meridian_opcode_total           counter   {realm,zone,shard,opcode}
//   meridian_opcode_dropped_total   counter   {realm,zone,shard,opcode,reason}
//   meridian_opcode_errors_total    counter   {realm,zone,shard,opcode}
//   meridian_action_rtt_seconds     histogram {realm,zone,shard,action}
//   meridian_db_queue_depth         gauge     {realm,db}
//   meridian_db_latency_seconds     histogram {realm,db}
//   meridian_aoi_entities           gauge     {realm,zone,shard,map}
//   meridian_grids_active           gauge     {realm,zone,shard,map}
//   meridian_instances_active       gauge     {realm}
//   meridian_movement_violations_total   counter {realm,zone,shard,kind}
//   meridian_movement_corrections_total  counter {realm,zone,shard}
//   meridian_disconnects_total      counter   {realm,reason}
//   meridian_reconnects_total       counter   {realm,outcome}
//   meridian_saves_batched_total    counter   {realm}
//   meridian_rss_bytes              gauge     {realm,process}
//   meridian_client_log_ingest_total counter  {realm,severity,build,platform}
//   meridian_client_crash_total     counter   {realm,build,platform}
//   (plus auth-outcome + connection signals — see below; these extend the
//    catalog's authd/net rows with the concrete series the M0 daemons emit.)
//
// Each accessor returns the get-or-created family from the default registry, so
// the FIRST caller declares HELP/labels and every later caller shares it.

#ifndef MERIDIAN_METRICS_CATALOG_H
#define MERIDIAN_METRICS_CATALOG_H

#include "meridian/metrics/registry.h"

namespace meridian::metrics::catalog {

// ── worldd: realm health ────────────────────────────────────────────────────

// Map tick loop duration (p99 = tick health). Dashboard: Realm health.
inline HistogramFamily& tick_duration_seconds() {
    return default_registry().histogram(
        "meridian_tick_duration_seconds", "Map tick loop duration in seconds (p99 = tick health)",
        {"realm", "zone", "shard", "map"});
}

// Concurrent connected users. Dashboard: Realm health.
inline GaugeFamily& ccu() {
    return default_registry().gauge("meridian_ccu", "Concurrent connected users",
                                    {"realm", "zone", "shard"});
}

// Sessions by lifecycle state. Dashboard: Realm health / Player experience.
inline GaugeFamily& sessions() {
    return default_registry().gauge("meridian_sessions", "Sessions by lifecycle state",
                                    {"realm", "state"});
}

// Per-opcode message rate. Dashboard: Realm health.
inline CounterFamily& opcode_total() {
    return default_registry().counter("meridian_opcode_total", "Per-opcode message rate",
                                      {"realm", "zone", "shard", "opcode"});
}

// Per-opcode drop rate. Dashboard: Errors.
inline CounterFamily& opcode_dropped_total() {
    return default_registry().counter("meridian_opcode_dropped_total", "Per-opcode drop rate",
                                      {"realm", "zone", "shard", "opcode", "reason"});
}

// Per-opcode error rate. Dashboard: Errors.
inline CounterFamily& opcode_errors_total() {
    return default_registry().counter("meridian_opcode_errors_total", "Per-opcode error rate",
                                      {"realm", "zone", "shard", "opcode"});
}

// Client→apply→client action round-trip. Dashboard: Player experience.
inline HistogramFamily& action_rtt_seconds() {
    return default_registry().histogram(
        "meridian_action_rtt_seconds", "Client action round-trip time in seconds",
        {"realm", "zone", "shard", "action"});
}

// Entities in AoI per map. Dashboard: Realm health.
inline GaugeFamily& aoi_entities() {
    return default_registry().gauge("meridian_aoi_entities", "Entities in AoI per map",
                                    {"realm", "zone", "shard", "map"});
}

// Active (ticking) grids per map. Dashboard: Realm health.
inline GaugeFamily& grids_active() {
    return default_registry().gauge("meridian_grids_active", "Active ticking grids per map",
                                    {"realm", "zone", "shard", "map"});
}

// Active dungeon/BG instances. Dashboard: Realm health.
inline GaugeFamily& instances_active() {
    return default_registry().gauge("meridian_instances_active", "Active dungeon/BG instances",
                                    {"realm"});
}

// Batched persistence writes. Dashboard: Realm health.
inline CounterFamily& saves_batched_total() {
    return default_registry().counter("meridian_saves_batched_total", "Batched persistence writes",
                                      {"realm"});
}

// ── worldd: player experience ───────────────────────────────────────────────

// Movement-check violations (speed/teleport/bounds/flag). Dashboard: Player exp / Errors.
inline CounterFamily& movement_violations_total() {
    return default_registry().counter(
        "meridian_movement_violations_total",
        "Movement-check violations by kind (speed/teleport/bounds/flag)",
        {"realm", "zone", "shard", "kind"});
}

// Snap-back corrections issued to clients. Dashboard: Player experience.
inline CounterFamily& movement_corrections_total() {
    return default_registry().counter("meridian_movement_corrections_total",
                                      "Snap-back movement corrections issued to clients",
                                      {"realm", "zone", "shard"});
}

// Disconnects by reason. Dashboard: Player experience.
inline CounterFamily& disconnects_total() {
    return default_registry().counter("meridian_disconnects_total", "Disconnects by reason",
                                      {"realm", "reason"});
}

// Reconnect attempts by outcome. Dashboard: Player experience.
inline CounterFamily& reconnects_total() {
    return default_registry().counter("meridian_reconnects_total", "Reconnect attempts by outcome",
                                      {"realm", "outcome"});
}

// ── worldd/authd: DB + resource ─────────────────────────────────────────────

// DB write-queue backlog. Dashboard: Realm health.
inline GaugeFamily& db_queue_depth() {
    return default_registry().gauge("meridian_db_queue_depth", "DB write-queue backlog",
                                    {"realm", "db"});
}

// DB round-trip latency. Dashboard: Realm health.
inline HistogramFamily& db_latency_seconds() {
    return default_registry().histogram("meridian_db_latency_seconds",
                                        "DB round-trip latency in seconds", {"realm", "db"});
}

// Process resident memory. Dashboard: Realm health.
inline GaugeFamily& rss_bytes() {
    return default_registry().gauge("meridian_rss_bytes", "Process resident memory in bytes",
                                    {"realm", "process"});
}

// ── authd: client ingest (count only — no payload; privacy §2a) ─────────────

// Client ERROR/CRITICAL log events received. Dashboard: Errors.
inline CounterFamily& client_log_ingest_total() {
    return default_registry().counter(
        "meridian_client_log_ingest_total",
        "Client ERROR/CRITICAL log events received (count only, no payload)",
        {"realm", "severity", "build", "platform"});
}

// Client crash reports received. Dashboard: Errors.
inline CounterFamily& client_crash_total() {
    return default_registry().counter("meridian_client_crash_total",
                                      "Client crash reports received (count only)",
                                      {"realm", "build", "platform"});
}

// ── authd: auth outcomes + SRP timing ───────────────────────────────────────
// The SAD §8.5 authd rows measure the login funnel: attempts, per-outcome
// results, and SRP handshake timing. These extend the catalog with the concrete
// series authd emits; `outcome` mirrors LoginOutcome (granted / rejected_hello /
// rejected_auth / rejected_realm / protocol_error / transport_closed).

// Login attempts received (one per accepted auth connection). Dashboard: Errors.
inline CounterFamily& auth_attempts_total() {
    return default_registry().counter("meridian_auth_attempts_total", "Login attempts received",
                                      {"realm"});
}

// Login outcomes by result. Dashboard: Player experience / Errors.
inline CounterFamily& auth_results_total() {
    return default_registry().counter("meridian_auth_results_total", "Login outcomes by result",
                                      {"realm", "outcome"});
}

// SRP handshake duration in seconds (login → grant, server side). Dashboard: Player experience.
inline HistogramFamily& auth_srp_duration_seconds() {
    return default_registry().histogram("meridian_auth_srp_duration_seconds",
                                        "SRP login handshake duration in seconds (server side)",
                                        {"realm", "outcome"});
}

// ── net: connection lifecycle ───────────────────────────────────────────────
// The libs/net accept/close seam (SAD §8.5 "connection accept/close"). `process`
// distinguishes authd vs worldd since both link the same net library.

// Connections accepted (post TLS 1.3 handshake). Dashboard: Realm health.
inline CounterFamily& connections_accepted_total() {
    return default_registry().counter("meridian_connections_accepted_total",
                                      "Connections accepted after TLS handshake",
                                      {"realm", "process"});
}

// Connections closed. Dashboard: Realm health.
inline CounterFamily& connections_closed_total() {
    return default_registry().counter("meridian_connections_closed_total", "Connections closed",
                                      {"realm", "process"});
}

}  // namespace meridian::metrics::catalog

#endif  // MERIDIAN_METRICS_CATALOG_H
