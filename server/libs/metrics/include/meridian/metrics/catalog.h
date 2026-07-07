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
//   meridian_db_latency_seconds     histogram {realm,db}
//   meridian_aoi_entities           gauge     {realm,zone,shard,map}
//   meridian_movement_violations_total   counter {realm,zone,shard,kind}
//   meridian_movement_corrections_total  counter {realm,zone,shard}
//   meridian_disconnects_total      counter   {realm,reason}
//   meridian_rss_bytes              gauge     {realm,process}
//   meridian_io_workers             gauge     {realm,process}   (#278)
//   meridian_io_workers_busy        gauge     {realm,process}   (#278)
//   meridian_client_log_ingest_total counter  {realm,severity,build,platform}
//   (plus auth-outcome + connection signals — see below; these extend the
//    catalog's authd/net rows with the concrete series the M0 daemons emit.)
//
// FUTURE (declared as name reservations, NOT emitted at M0 — see #297):
//   meridian_grids_active / _instances_active / _saves_batched_total  (M1 world)
//   meridian_client_crash_total   (blocked on client crashpad #109; Sentry sink)
//
// REMOVED at M0 (#297 — declared but no emit path; would be permanent "No data"):
//   meridian_action_rtt_seconds   (no action-apply loop until M1)
//   meridian_reconnects_total     (reconnect is client-side #96; no server path)
//   meridian_db_queue_depth       (M0 DB layer is synchronous; no queue)
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

// NOTE (#297): meridian_action_rtt_seconds was declared here but never emitted —
// there is no client→apply→client action-apply loop at M0 (it lands with the M1
// combat/action pipeline). Removed rather than shipped as a permanently-empty
// series (a "No data" panel is worse than an absent one). Re-add the accessor
// alongside the emit site when the action loop exists.

// Entities in AoI per map. Dashboard: Realm health.
inline GaugeFamily& aoi_entities() {
    return default_registry().gauge("meridian_aoi_entities", "Entities in AoI per map",
                                    {"realm", "zone", "shard", "map"});
}

// ── FUTURE (M1) — declared name reservations, NOT emitted at M0 (#297) ────────
// These name the worldd grid/instance/persistence signals but there is no emit
// path yet (grids, dungeon/BG instances, and batched persistence all arrive with
// the M1 world/persistence work). They are kept as forward-looking name
// reservations so the series names stay pinned in this one catalog, but NO M0
// dashboard or alert queries them (that would be a permanent "No data" panel /
// never-firing rule — see server/ops). Wire the emit site + the panel together
// when the feature lands.

// Active (ticking) grids per map. FUTURE (M1) — not emitted at M0.
inline GaugeFamily& grids_active() {
    return default_registry().gauge("meridian_grids_active", "Active ticking grids per map",
                                    {"realm", "zone", "shard", "map"});
}

// Active dungeon/BG instances. FUTURE (M1) — not emitted at M0.
inline GaugeFamily& instances_active() {
    return default_registry().gauge("meridian_instances_active", "Active dungeon/BG instances",
                                    {"realm"});
}

// Batched persistence writes. FUTURE (M1) — not emitted at M0.
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

// NOTE (#297): meridian_reconnects_total was declared here but never emitted. At
// M0 reconnect is a CLIENT-side concern (#96): on a drop the client re-runs the
// login funnel within the grant's reconnect window (auth.fbs
// SessionGrant.reconnect_window_ms) — there is no server-side "resume session"
// path that would count reconnect outcomes. Removed rather than shipped as a
// dead series; re-add with the server resume path if one ever exists.

// ── worldd/authd: DB + resource ─────────────────────────────────────────────

// NOTE (#297): meridian_db_queue_depth was declared here but never emitted. The
// M0 DB layer (server/libs/db) is a SYNCHRONOUS connection with no write queue
// to measure — the async worker-pool + per-DB connection pools the SAD §2.2
// describes (and that a queue-depth gauge measures) wrap this layer at M1.
// Removed rather than shipped as a constant-absent series; re-add with the
// async DB queue.

// DB round-trip latency. Dashboard: Realm health.
inline HistogramFamily& db_latency_seconds() {
    return default_registry().histogram("meridian_db_latency_seconds",
                                        "DB round-trip latency in seconds", {"realm", "db"});
}

// Process resident memory. Dashboard: Realm health / worldd / Realm overview.
// Emitted live by RssSampler in authd + worldd (see rss_sampler.h).
inline GaugeFamily& rss_bytes() {
    return default_registry().gauge("meridian_rss_bytes", "Process resident memory in bytes",
                                    {"realm", "process"});
}

// ── worldd: IO-worker pool saturation (#278) ────────────────────────────────
// worldd serves each connection start-to-finish on one blocking IO worker; the
// pool auto-sizes to hardware_concurrency-3 (override --io-workers N). Max
// concurrent CCU per worldd ≈ pool size, so pool size + in-flight count is the
// real saturation signal (busy/pool ≈ 1 ⇒ new connections wait in the accept
// backlog). Dashboard: worldd (IO-worker utilization).

// Configured IO-worker pool size (constant per run). {process} distinguishes
// worldd (only worldd runs an IO-worker pool at M0).
inline GaugeFamily& io_workers() {
    return default_registry().gauge("meridian_io_workers", "worldd IO-worker pool size",
                                    {"realm", "process"});
}

// IO-workers currently serving a connection (in-flight). busy/pool = utilization.
inline GaugeFamily& io_workers_busy() {
    return default_registry().gauge("meridian_io_workers_busy",
                                    "worldd IO-workers currently serving a connection",
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

// Client crash reports received. FUTURE — not emitted at M0 (#297). BLOCKED ON
// the client crash handler (crashpad, #109): until the client captures and ships
// crash reports there is nothing to count. Per telemetry-privacy.md §4 the crash
// SINK is a Sentry-compatible endpoint, NOT this Prometheus stack — this counter
// is only the server-side received-count once #109 lands. Kept as a name
// reservation; no M0 dashboard/alert queries it. Wire the emit site + the panel
// together when #109 ships.
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
