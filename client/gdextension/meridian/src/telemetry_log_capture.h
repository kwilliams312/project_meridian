// Project Meridian — client telemetry log capture engine (issue #168).
//
// The engine-free CORE that implements the D-29 client log channel policy: it
// accepts log events, enforces the ERROR/CRITICAL gate, the opt-out flag, the
// rate limit and the batching, attaches the (no-PII) session/build/platform
// context, and hands finished batches to an ITransport (serialized to the
// Sentry-compatible envelope). NO Godot — plain C++17 (Client SAD §9.2), so the
// #168 doctest drives it directly with a MockTransport.
//
// Threading note: this core is NOT internally threaded — it is deterministic and
// clock-injected so tests are reproducible (time is passed in, not read from a
// system clock). The GDExtension binding (meridian_telemetry.*) owns the "never
// block the game loop" concern: it calls capture() on the logger thread (cheap:
// gate + push under a small lock) and drains/ships batches off the main loop.
// The transport itself is what runs async (issue: "async/queued"); this core's
// job is the policy, which must be exact and testable.

#ifndef MERIDIAN_TELEMETRY_LOG_CAPTURE_H
#define MERIDIAN_TELEMETRY_LOG_CAPTURE_H

#include "telemetry_log_core.h"
#include "telemetry_transport.h"

#include <cstdint>
#include <vector>

namespace meridian::telemetry {

// The capture pipeline. Own one per client. Feed it CaptureConfig + a
// SessionContext at construction; call capture() for every log line the sink
// sees; call flush_due()/flush() to emit batches; ship batches through an
// ITransport. All time is EXPLICIT (ms) so the pipeline is deterministic under
// test — the binding passes real wall-clock ms.
class LogCapture {
public:
	LogCapture(CaptureConfig config, SessionContext context);

	// ── Capture ────────────────────────────────────────────────────────────
	// Offer one log event to the pipeline. Returns true if it was CAPTURED
	// (buffered for shipping), false if it was DROPPED. An event is dropped when:
	//   • opt-out is off (enabled == false)              — nothing is captured
	//   • severity < Error (TRACE/DEBUG/INFO/WARN)        — never leaves device
	//   • the rate limit for the current window is exceeded (storm suppression;
	//     the drop is COUNTED and surfaces on the next batch)
	// `now_ms` is the client wall-clock (epoch ms) at capture — also used as the
	// event timestamp when the event's own timestamp_ms is 0.
	bool capture(const LogEvent &event, uint64_t now_ms);

	// Convenience overload: capture by severity + message directly.
	bool capture(Severity severity, const std::string &message,
	             const std::string &logger, uint64_t now_ms);

	// ── Batching ─────────────────────────────────────────────────────────────
	// True when a batch is due: buffer reached batch_max_events, OR
	// batch_flush_interval_ms elapsed since the first buffered event, OR there is
	// a pending rate-limit-drop summary to report. Cheap — call it each frame.
	bool flush_due(uint64_t now_ms) const;

	// Take the currently buffered events (and the rate-limit drop count) as one
	// LogBatch, clearing the buffer. Returns an empty batch if nothing is
	// pending. Does NOT ship — the caller ships (so shipping stays off the hot
	// path and the core has no transport dependency at flush time).
	LogBatch flush();

	// ── Shipping ─────────────────────────────────────────────────────────────
	// Serialize `batch` and hand it to `transport`. Returns the transport result.
	// On Failed, the batch's events are RE-QUEUED at the front of the buffer for
	// the next attempt (bounded by batch_max_events * a small factor so a dead
	// endpoint can't grow memory without limit) — this is the back-off/retry
	// accounting; the actual back-off DELAY is the transport's concern (it runs
	// async). An empty batch is a no-op (returns Ok). Opt-out short-circuits:
	// when disabled, nothing is ever shipped.
	ShipResult ship(const LogBatch &batch, ITransport &transport);

	// Convenience: flush_due? -> flush -> ship, in one call. Returns true if a
	// batch was shipped (or attempted), false if nothing was due.
	bool flush_and_ship(ITransport &transport, uint64_t now_ms);

	// ── Config / introspection ───────────────────────────────────────────────
	void set_enabled(bool enabled) { config_.enabled = enabled; }
	bool enabled() const { return config_.enabled; }
	const SessionContext &context() const { return context_; }
	std::size_t buffered_count() const { return buffer_.size(); }
	uint32_t pending_dropped() const { return dropped_by_rate_limit_; }
	// Total events captured / dropped over the pipeline's lifetime (diagnostics).
	uint64_t lifetime_captured() const { return lifetime_captured_; }
	uint64_t lifetime_dropped() const { return lifetime_dropped_; }

private:
	// Sliding-window rate limiter: returns true if a capture is allowed in the
	// window containing now_ms, advancing the window as time passes.
	bool rate_limit_allows(uint64_t now_ms);

	CaptureConfig  config_;
	SessionContext context_;

	std::vector<LogEvent> buffer_;
	uint64_t first_buffered_ms_ = 0;   // when the current batch's first event landed
	bool     have_first_ = false;

	// Rate-limit window state.
	uint64_t window_start_ms_ = 0;
	uint32_t window_count_    = 0;
	bool     have_window_     = false;

	// Rate-limit drops not yet reported on a batch.
	uint32_t dropped_by_rate_limit_ = 0;

	// Lifetime counters (diagnostics only).
	uint64_t lifetime_captured_ = 0;
	uint64_t lifetime_dropped_  = 0;

	// Cap on re-queued events so a dead endpoint can't grow memory unbounded.
	std::size_t requeue_cap() const {
		return static_cast<std::size_t>(config_.batch_max_events) * 4;
	}
};

} // namespace meridian::telemetry

#endif // MERIDIAN_TELEMETRY_LOG_CAPTURE_H
