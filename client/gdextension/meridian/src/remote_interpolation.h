// Project Meridian — client REMOTE-entity interpolation + clock-sync estimator
// CORE (issue #104).
//
// ENGINE-FREE by design (Client SAD §9.2 "engine-agnostic cores", the same
// #102/#168 discipline): this header and its .cpp contain NO Godot types. The
// GDExtension node (`MeridianRemoteInterpolator`,
// meridian_remote_interpolator.*) is a thin wrapper that marshals Godot
// state in and out of these plain structs. Keeping it engine-free lets a plain
// C++17 doctest inject a clock + snapshots and assert deterministically (SAD
// §11 client doctest), and lets the headless bot (#111) feed it the EntityUpdate
// frames it already captures (bot_core.cpp capture_entity_frame) with no engine.
//
// ─────────────────────────────────────────────────────────────────────────
//  WHAT THIS IS (and is NOT)
// ─────────────────────────────────────────────────────────────────────────
// This is the REMOTE side of movement — smoothing OTHER players' motion. It is
// DISTINCT from the #102 local-player prediction/reconciliation
// (movement_controller.*): that predicts the LOCAL player's own motion from its
// own input and reconciles against server acks. Here we have no input for the
// entity — we only receive the server's authoritative MovementState/EntityUpdate
// samples (schema/net/world.fbs EntityUpdate — position + server_time_ms) at a
// low, jittery rate (~10-20 Hz via #87 AoI relay), and must render a smooth
// position between them.
//
// Two cooperating pieces (Client SAD §2.2 "Interpolation clock", §5.1 clock sync):
//
//   1. ClockEstimator — a filtered client↔server clock-offset + RTT estimate,
//      fed by ClockSync (#65) round-trips (or, failing that, seeded from raw
//      MovementState.server_time_ms). It maps a server timestamp onto the
//      client's monotonic timeline so buffered snapshots can be time-aligned.
//      Robust to outliers (median-of-recent gate + EWMA smoothing).
//
//   2. SnapshotBuffer / RemoteInterpolator — per remote entity, a small ring of
//      (server_time_ms, position) snapshots. Rendered position is sampled at
//      `estimated_server_now - interp_delay`, LINEARLY interpolated between the
//      two bracketing snapshots. A fixed interp delay (kInterpDelayMs = 100 ms =
//      2× the 50 ms server tick, Client SAD §2.2 "~2× server tick") absorbs
//      arrival jitter: as long as the next snapshot arrives before the render
//      cursor catches the last one, rendering never stalls. Gaps beyond the
//      buffer are extrapolated up to kExtrapolationCapMs (~250 ms, SAD §2.2)
//      then frozen. Out-of-order / duplicate snapshots are handled by the buffer
//      (insert-sorted, dedup by timestamp).

#ifndef MERIDIAN_REMOTE_INTERPOLATION_H
#define MERIDIAN_REMOTE_INTERPOLATION_H

#include "movement_controller.h"   // meridian::movement::Vec3 (shared value type)

#include <cstdint>
#include <deque>
#include <optional>
#include <unordered_map>

namespace meridian::remote {

using meridian::movement::Vec3;

// ===========================================================================
// Timing constants — [SAD-LOCKED] Client SAD §2.2 "Interpolation clock".
// ===========================================================================
// remote entities rendered at `server_time − interp_delay (~2× server tick)`,
// "extrapolation capped at ~250 ms then freeze/fade". The 50 ms server tick is
// the LOCKED cadence (movement_constants.h kTickMillis). 2× that = 100 ms of
// delay buffer: it holds two snapshots' worth of slack, so a snapshot can arrive
// up to ~100 ms late (one full missed relay at 10 Hz) and still be in the buffer
// before the render cursor needs it. Larger delay = smoother under jitter but
// more visible lag; 100 ms is the SAD's balance for M0.
inline constexpr uint64_t kInterpDelayMs      = 2u * static_cast<uint64_t>(
                                                    meridian::movement::kTickMillis); // 100 ms
inline constexpr uint64_t kExtrapolationCapMs = 250u;   // SAD §2.2 "capped at ~250 ms then freeze"

// ===========================================================================
// ClockEstimator — filtered client↔server clock offset + RTT (Client SAD §5.1).
// ===========================================================================
// A ClockSync (#65) round-trip is: client stamps t0 (client_time_ms) and sends;
// server replies stamping ts (server_time_ms); client receives at t1. Then, for
// a symmetric path:
//
//     rtt    = t1 - t0
//     offset = ts - (t0 + t1) / 2         // server_clock - client_clock
//
// so `server_time ≈ client_time + offset`. The instantaneous offset is noisy
// (asymmetric latency, scheduling). We keep a bounded window of recent samples
// and combine two robustness layers:
//   • OUTLIER GATE: reject a sample whose measured offset deviates from the
//     current median-of-window by more than kOutlierRejectMs — a spike (a
//     one-off delayed reply) never yanks the estimate.
//   • EWMA SMOOTH: fold accepted samples into an exponential moving average so
//     the estimate tracks slow clock drift without chasing per-sample noise.
//
// Everything is injectable: the caller passes the three timestamps, so a test
// drives it with a synthetic offset + noise and asserts convergence. No wall
// clock is read inside.

class ClockEstimator {
public:
	ClockEstimator() = default;

	// Fold one ClockSync round-trip. t0 = client send stamp, ts = server reply
	// stamp, t1 = client receive stamp (all monotonic ms in their own clocks).
	// Returns true if the sample was ACCEPTED (passed the outlier gate), false if
	// rejected as an outlier. The first kMinSamplesBeforeGate samples always seed
	// the window (no gate yet — we have nothing to compare against).
	bool add_round_trip(uint64_t client_send_ms, uint64_t server_reply_ms,
	                    uint64_t client_recv_ms);

	// True once at least one sample has been folded (offset is meaningful).
	bool has_estimate() const { return have_estimate_; }

	// Smoothed server_clock - client_clock, in ms (signed: server may be behind).
	int64_t offset_ms() const { return smoothed_offset_ms_; }

	// Smoothed round-trip time (ms). Half of this is the one-way latency estimate.
	int64_t rtt_ms() const { return smoothed_rtt_ms_; }

	// Map a client monotonic time onto the estimated server timeline (and back).
	// Before any estimate exists these are identity (offset 0) — safe default.
	uint64_t client_to_server(uint64_t client_ms) const {
		return static_cast<uint64_t>(static_cast<int64_t>(client_ms) + smoothed_offset_ms_);
	}
	uint64_t server_to_client(uint64_t server_ms) const {
		return static_cast<uint64_t>(static_cast<int64_t>(server_ms) - smoothed_offset_ms_);
	}

	std::size_t sample_count() const { return window_.size(); }

	// --- Tunables (public constexpr so tests + the wrapper can cite them) ---
	// EWMA weight for a freshly accepted sample. 0.2 => the estimate reaches
	// ~90% of a step change in ~10 samples: fast enough to lock on at startup,
	// slow enough to reject residual noise.
	static constexpr double  kEwmaAlpha            = 0.2;
	// Window of recent RAW offsets kept for the median outlier gate.
	static constexpr std::size_t kWindow           = 16;
	// A sample whose raw offset is more than this from the window median is
	// rejected. Sized to swallow typical LAN/Wi-Fi asymmetry but catch a stall.
	static constexpr int64_t kOutlierRejectMs      = 150;
	// Below this many samples we cannot form a trustworthy median, so we accept
	// unconditionally (bootstrap).
	static constexpr std::size_t kMinSamplesBeforeGate = 4;

private:
	int64_t median_offset() const;  // median of window_ (raw offsets)

	std::deque<int64_t> window_;          // recent RAW offsets (for median gate)
	int64_t smoothed_offset_ms_ = 0;      // EWMA of accepted offsets
	int64_t smoothed_rtt_ms_    = 0;      // EWMA of accepted rtts
	bool    have_estimate_      = false;
};

// ===========================================================================
// A single buffered snapshot of a remote entity (what EntityUpdate carries).
// ===========================================================================
struct Snapshot {
	uint64_t server_time_ms = 0;   // authoritative timestamp (MovementState/ClockSync-keyed)
	Vec3     position;
	float    orientation = 0.0f;
};

// The result of sampling the interpolator at a render time — the position to
// draw plus WHY (for tests / presentation-fade decisions).
enum class SampleKind : uint8_t {
	kEmpty        = 0,   // no snapshots buffered — nothing to render
	kInterpolated = 1,   // between two bracketing snapshots (the healthy case)
	kExtrapolated = 2,   // ahead of newest snapshot but within the extrap cap
	kHeld         = 3,   // clamped to newest (extrap cap exceeded) or before oldest
};

struct SampleResult {
	SampleKind kind = SampleKind::kEmpty;
	Vec3       position;
	float      orientation = 0.0f;
};

// ===========================================================================
// SnapshotBuffer — one remote entity's snapshot ring + the interpolation query.
// ===========================================================================
// Insert-sorted by server_time_ms; duplicates (same timestamp) replace in place
// (a corrected resend wins). `sample(render_server_ms)` finds the bracketing
// pair and lerps.
class SnapshotBuffer {
public:
	// Insert a snapshot. Handles out-of-order arrival (insert-sorted) and exact-
	// timestamp duplicates (last write wins for that timestamp — a corrected
	// resend replaces the prior). Trims to kMaxSnapshots oldest-first.
	void push(const Snapshot& snap);

	// Sample the interpolated state at server time `render_server_ms` (already
	// = estimated_server_now - interp_delay; the interpolator applies the delay).
	// Linear interpolation between the two bracketing snapshots. If the render
	// time is:
	//   • before the oldest snapshot  → hold at oldest (kHeld);
	//   • between two snapshots        → lerp (kInterpolated);
	//   • after the newest snapshot    → extrapolate along the last segment's
	//     velocity up to kExtrapolationCapMs past newest (kExtrapolated), then
	//     hold at newest (kHeld);
	//   • no snapshots at all          → kEmpty.
	SampleResult sample(uint64_t render_server_ms) const;

	std::size_t size() const { return snaps_.size(); }
	bool empty() const { return snaps_.empty(); }
	std::optional<uint64_t> newest_time() const {
		if (snaps_.empty()) return std::nullopt;
		return snaps_.back().server_time_ms;
	}
	std::optional<uint64_t> oldest_time() const {
		if (snaps_.empty()) return std::nullopt;
		return snaps_.front().server_time_ms;
	}

	// Keep at most this many snapshots per entity. At 100 ms delay + ~10 Hz
	// updates the working set is ~2-3; 16 leaves generous slack for a burst and
	// bounds memory for a crowded AoI.
	static constexpr std::size_t kMaxSnapshots = 16;

private:
	std::deque<Snapshot> snaps_;   // ascending server_time_ms
};

// ===========================================================================
// RemoteInterpolator — the per-session facade: a registry of remote entities
// keyed by server GUID, plus the shared ClockEstimator. This is the object the
// client/bot feeds EntityEnter/Update/Leave + ClockSync into, and queries
// interpolated positions from (the wrapper mirrors these methods to Godot).
// ===========================================================================
class RemoteInterpolator {
public:
	RemoteInterpolator() = default;

	// --- Clock sync (fed from ClockSync #65 round-trips) ---
	// Returns true if the sample was accepted (not an outlier).
	bool on_clock_sync(uint64_t client_send_ms, uint64_t server_reply_ms,
	                   uint64_t client_recv_ms) {
		return clock_.add_round_trip(client_send_ms, server_reply_ms, client_recv_ms);
	}
	const ClockEstimator& clock() const { return clock_; }

	// --- Entity lifecycle (fed from #87 AoI relay the bot captures) ---
	// EntityEnter: start tracking `guid` and seed its first snapshot at the enter
	// position/time (so it renders immediately at the spawn point, no empty frame).
	void on_enter(uint64_t guid, const Vec3& position, float orientation,
	              uint64_t server_time_ms);

	// EntityUpdate: buffer a new authoritative snapshot. If the entity isn't
	// tracked yet (update before enter, e.g. a late-join relay), it starts
	// tracking it — defensive: never drop a positional update.
	void on_update(uint64_t guid, const Vec3& position, float orientation,
	               uint64_t server_time_ms);

	// EntityLeave: stop tracking (drops the buffer). Presentation-fade is above
	// this core (SAD §2.2); here we simply forget the entity.
	void on_leave(uint64_t guid);

	bool is_tracked(uint64_t guid) const { return entities_.count(guid) != 0; }
	std::size_t tracked_count() const { return entities_.size(); }

	// --- Rendering query ---
	// Sample entity `guid`'s interpolated state for a frame whose CLIENT-clock
	// time is `client_now_ms`. Internally: map to server time via the clock
	// estimator, subtract kInterpDelayMs, sample the buffer. Returns kEmpty if
	// the guid isn't tracked or has no snapshots.
	SampleResult sample_entity(uint64_t guid, uint64_t client_now_ms) const;

	// The render server time this interpolator would sample at for a given client
	// time (= estimated_server_now - interp_delay). Exposed for tests.
	uint64_t render_server_time(uint64_t client_now_ms) const;

private:
	ClockEstimator clock_;
	std::unordered_map<uint64_t, SnapshotBuffer> entities_;
};

} // namespace meridian::remote

#endif // MERIDIAN_REMOTE_INTERPOLATION_H
