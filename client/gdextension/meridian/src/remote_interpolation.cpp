// Project Meridian — remote-entity interpolation + clock-sync estimator CORE
// implementation (issue #104). Engine-free; see remote_interpolation.h for the
// design rationale + SAD citations.

#include "remote_interpolation.h"

#include <algorithm>
#include <vector>

namespace meridian::remote {

// ===========================================================================
// Small vector helpers (engine-free — Vec3 is the shared movement value type).
// ===========================================================================
namespace {

Vec3 lerp(const Vec3& a, const Vec3& b, float t) {
	return Vec3{
		a.x + (b.x - a.x) * t,
		a.y + (b.y - a.y) * t,
		a.z + (b.z - a.z) * t,
	};
}

// Shortest-arc interpolation for a facing angle (radians). Angles wrap at 2π;
// a naive lerp from 350° to 10° would spin the long way round. Wrap the delta
// into (-π, π] first so the entity turns the short way.
float lerp_angle(float a, float b, float t) {
	constexpr float kPi  = 3.14159265358979323846f;
	constexpr float kTau = 2.0f * kPi;
	float d = b - a;
	while (d >  kPi) d -= kTau;
	while (d <= -kPi) d += kTau;
	return a + d * t;
}

// Extrapolate a position PAST snapshot `b` using the velocity of the (a→b)
// segment, for `dt_ms` beyond b. Guards a zero-duration segment (avoid /0).
Vec3 extrapolate(const Snapshot& a, const Snapshot& b, uint64_t dt_ms) {
	const uint64_t span = b.server_time_ms - a.server_time_ms;
	if (span == 0) return b.position;                    // degenerate — hold
	const float k = static_cast<float>(dt_ms) / static_cast<float>(span);
	return Vec3{
		b.position.x + (b.position.x - a.position.x) * k,
		b.position.y + (b.position.y - a.position.y) * k,
		b.position.z + (b.position.z - a.position.z) * k,
	};
}

} // namespace

// ===========================================================================
// ClockEstimator
// ===========================================================================

int64_t ClockEstimator::median_offset() const {
	// Copy + nth_element for an exact median of the raw-offset window. Window is
	// tiny (<= kWindow == 16) so this is cheap and called only on the outlier gate.
	std::vector<int64_t> tmp(window_.begin(), window_.end());
	const std::size_t n = tmp.size();
	std::nth_element(tmp.begin(), tmp.begin() + n / 2, tmp.end());
	int64_t hi = tmp[n / 2];
	if (n % 2 == 1) return hi;
	// even count: average the two central order statistics.
	std::nth_element(tmp.begin(), tmp.begin() + (n / 2 - 1), tmp.end());
	int64_t lo = tmp[n / 2 - 1];
	return (lo + hi) / 2;
}

bool ClockEstimator::add_round_trip(uint64_t client_send_ms,
                                    uint64_t server_reply_ms,
                                    uint64_t client_recv_ms) {
	// rtt = t1 - t0; offset = ts - (t0 + t1)/2. Compute in signed arithmetic —
	// the client and server clocks are independent, so ts may be well above or
	// below the client stamps; a naive unsigned subtract would wrap.
	const int64_t t0 = static_cast<int64_t>(client_send_ms);
	const int64_t ts = static_cast<int64_t>(server_reply_ms);
	const int64_t t1 = static_cast<int64_t>(client_recv_ms);

	int64_t rtt = t1 - t0;
	if (rtt < 0) rtt = 0;                       // clamp a non-monotonic recv stamp
	const int64_t midpoint = t0 + rtt / 2;      // == (t0 + t1)/2 for rtt = t1-t0
	const int64_t raw_offset = ts - midpoint;

	// OUTLIER GATE (only once we have enough of a window to trust a median).
	if (window_.size() >= kMinSamplesBeforeGate) {
		const int64_t med = median_offset();
		int64_t dev = raw_offset - med;
		if (dev < 0) dev = -dev;
		if (dev > kOutlierRejectMs) {
			// Reject: do NOT fold into the estimate and do NOT pollute the window.
			return false;
		}
	}

	// Accept: push into the median window (bounded), then EWMA-smooth.
	window_.push_back(raw_offset);
	if (window_.size() > kWindow) window_.pop_front();

	if (!have_estimate_) {
		// First accepted sample seeds the EWMA directly (no ramp from 0).
		smoothed_offset_ms_ = raw_offset;
		smoothed_rtt_ms_    = rtt;
		have_estimate_      = true;
	} else {
		const double a = kEwmaAlpha;
		smoothed_offset_ms_ = static_cast<int64_t>(
			a * static_cast<double>(raw_offset) +
			(1.0 - a) * static_cast<double>(smoothed_offset_ms_));
		smoothed_rtt_ms_ = static_cast<int64_t>(
			a * static_cast<double>(rtt) +
			(1.0 - a) * static_cast<double>(smoothed_rtt_ms_));
	}
	return true;
}

// ===========================================================================
// SnapshotBuffer
// ===========================================================================

void SnapshotBuffer::push(const Snapshot& snap) {
	// Common case: strictly newer than everything → append (O(1)).
	if (snaps_.empty() || snap.server_time_ms > snaps_.back().server_time_ms) {
		snaps_.push_back(snap);
	} else {
		// Out-of-order or duplicate. Find the insertion point by timestamp.
		auto it = std::lower_bound(
			snaps_.begin(), snaps_.end(), snap.server_time_ms,
			[](const Snapshot& s, uint64_t t) { return s.server_time_ms < t; });
		if (it != snaps_.end() && it->server_time_ms == snap.server_time_ms) {
			*it = snap;   // exact-timestamp duplicate → last write wins (corrected resend)
		} else {
			snaps_.insert(it, snap);   // stale-but-new-timestamp → slot into order
		}
	}
	// Trim oldest-first to bound memory.
	while (snaps_.size() > kMaxSnapshots) snaps_.pop_front();
}

SampleResult SnapshotBuffer::sample(uint64_t render_server_ms) const {
	SampleResult out;
	if (snaps_.empty()) {
		out.kind = SampleKind::kEmpty;
		return out;
	}

	// Before the oldest snapshot: hold at the oldest (we cannot interpolate into
	// the past we never had). This is the startup / just-entered case.
	const Snapshot& oldest = snaps_.front();
	if (render_server_ms <= oldest.server_time_ms) {
		out.kind        = (snaps_.size() == 1 && render_server_ms == oldest.server_time_ms)
		                      ? SampleKind::kInterpolated : SampleKind::kHeld;
		out.position    = oldest.position;
		out.orientation = oldest.orientation;
		return out;
	}

	const Snapshot& newest = snaps_.back();
	if (render_server_ms >= newest.server_time_ms) {
		// Ahead of the newest snapshot: a gap (no fresh update arrived in time).
		const uint64_t ahead = render_server_ms - newest.server_time_ms;
		if (ahead <= kExtrapolationCapMs && snaps_.size() >= 2) {
			// Extrapolate along the last segment's velocity — brief, bounded.
			const Snapshot& prev = snaps_[snaps_.size() - 2];
			out.kind        = SampleKind::kExtrapolated;
			out.position    = extrapolate(prev, newest, ahead);
			out.orientation = newest.orientation;   // hold facing (no angular vel model)
			return out;
		}
		// Beyond the cap (or only one snapshot to extrapolate from): freeze/hold.
		out.kind        = SampleKind::kHeld;
		out.position    = newest.position;
		out.orientation = newest.orientation;
		return out;
	}

	// Healthy case: render time brackets a pair. Find the segment [lo, hi] with
	// lo.t <= render < hi.t and lerp.
	// upper_bound gives the first snapshot strictly AFTER render_server_ms == hi.
	auto hi_it = std::upper_bound(
		snaps_.begin(), snaps_.end(), render_server_ms,
		[](uint64_t t, const Snapshot& s) { return t < s.server_time_ms; });
	// hi_it is valid (render < newest guaranteed above) and not begin() (render >
	// oldest guaranteed above), so lo = hi-1 is valid.
	const Snapshot& hi = *hi_it;
	const Snapshot& lo = *(hi_it - 1);

	const uint64_t span = hi.server_time_ms - lo.server_time_ms;
	const float t = (span == 0) ? 0.0f
	              : static_cast<float>(render_server_ms - lo.server_time_ms) /
	                static_cast<float>(span);

	out.kind        = SampleKind::kInterpolated;
	out.position    = lerp(lo.position, hi.position, t);
	out.orientation = lerp_angle(lo.orientation, hi.orientation, t);
	return out;
}

// ===========================================================================
// RemoteInterpolator
// ===========================================================================

void RemoteInterpolator::on_enter(uint64_t guid, const Vec3& position,
                                  float orientation, uint64_t server_time_ms) {
	SnapshotBuffer& buf = entities_[guid];   // create-or-get
	buf.push(Snapshot{server_time_ms, position, orientation});
}

void RemoteInterpolator::on_update(uint64_t guid, const Vec3& position,
                                   float orientation, uint64_t server_time_ms) {
	// entities_[guid] create-or-gets: an update before enter still starts tracking
	// (defensive — never drop a positional relay).
	SnapshotBuffer& buf = entities_[guid];
	buf.push(Snapshot{server_time_ms, position, orientation});
}

void RemoteInterpolator::on_leave(uint64_t guid) {
	entities_.erase(guid);
}

uint64_t RemoteInterpolator::render_server_time(uint64_t client_now_ms) const {
	const uint64_t server_now = clock_.client_to_server(client_now_ms);
	// Guard the subtraction: at startup server_now may be < the delay.
	return (server_now > kInterpDelayMs) ? (server_now - kInterpDelayMs) : 0;
}

SampleResult RemoteInterpolator::sample_entity(uint64_t guid,
                                               uint64_t client_now_ms) const {
	auto it = entities_.find(guid);
	if (it == entities_.end()) return SampleResult{};   // kEmpty
	return it->second.sample(render_server_time(client_now_ms));
}

} // namespace meridian::remote
