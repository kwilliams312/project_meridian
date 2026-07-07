// Project Meridian — telemetry log capture engine (issue #168).
// Implements the D-29 client log-channel policy: ERROR/CRITICAL gate, opt-out,
// sliding-window rate limit, size/time batching, and serialized shipping through
// an ITransport. Engine-free, deterministic (clock injected), zero deps.

#include "telemetry_log_capture.h"

#include <algorithm>

namespace meridian::telemetry {

LogCapture::LogCapture(CaptureConfig config, SessionContext context)
	: config_(config), context_(std::move(context)) {}

bool LogCapture::rate_limit_allows(uint64_t now_ms) {
	// A window of length rate_limit_window_ms admits at most rate_limit_max_events
	// captures. When now_ms passes the current window's end, the window resets.
	if (!have_window_ || now_ms - window_start_ms_ >= config_.rate_limit_window_ms) {
		window_start_ms_ = now_ms;
		window_count_    = 0;
		have_window_     = true;
	}
	if (window_count_ >= config_.rate_limit_max_events) {
		return false;
	}
	++window_count_;
	return true;
}

bool LogCapture::capture(const LogEvent &event, uint64_t now_ms) {
	// (1) Opt-out is honored FIRST — off ⇒ capture nothing (privacy §5). We do
	// not even count it; disabled means the pipeline is silent.
	if (!config_.enabled) {
		return false;
	}

	// (2) ERROR/CRITICAL gate — TRACE/DEBUG/INFO/WARN never leave the device
	// (privacy §2a). Dropped here are NOT counted as rate-limit drops; they are
	// simply out of scope for the channel.
	if (!is_shippable(event.severity)) {
		return false;
	}

	// (3) Rate limit — a storm beyond the window cap is dropped + COUNTED so the
	// suppression is observable on the next batch (D-29 rule 2).
	if (!rate_limit_allows(now_ms)) {
		++dropped_by_rate_limit_;
		++lifetime_dropped_;
		return false;
	}

	// Captured. Buffer it, stamping the timestamp if the caller left it 0.
	LogEvent stored = event;
	if (stored.timestamp_ms == 0) {
		stored.timestamp_ms = now_ms;
	}
	if (!have_first_) {
		first_buffered_ms_ = now_ms;
		have_first_        = true;
	}
	buffer_.push_back(std::move(stored));
	++lifetime_captured_;
	return true;
}

bool LogCapture::capture(Severity severity, const std::string &message,
                         const std::string &logger, uint64_t now_ms) {
	LogEvent ev;
	ev.severity     = severity;
	ev.message      = message;
	ev.logger       = logger;
	ev.timestamp_ms = now_ms;
	return capture(ev, now_ms);
}

bool LogCapture::flush_due(uint64_t now_ms) const {
	if (!config_.enabled) {
		return false;
	}
	// Size trigger.
	if (buffer_.size() >= config_.batch_max_events) {
		return true;
	}
	// Time trigger — bounded latency even for a trickle of events.
	if (have_first_ && now_ms - first_buffered_ms_ >= config_.batch_flush_interval_ms) {
		return true;
	}
	// A pending rate-limit-drop summary should also be reported.
	if (!buffer_.empty() && dropped_by_rate_limit_ > 0) {
		return true;
	}
	return false;
}

LogBatch LogCapture::flush() {
	LogBatch batch;
	batch.context               = context_;
	batch.events                = std::move(buffer_);
	batch.dropped_by_rate_limit = dropped_by_rate_limit_;

	buffer_.clear();
	have_first_            = false;
	dropped_by_rate_limit_ = 0;
	return batch;
}

ShipResult LogCapture::ship(const LogBatch &batch, ITransport &transport) {
	// Opt-out short-circuit: never ship when disabled (privacy §5).
	if (!config_.enabled) {
		return ShipResult::NoSink;
	}
	if (batch.empty()) {
		return ShipResult::Ok;
	}

	const std::string envelope = serialize_envelope(batch);
	const ShipResult result = transport.ship(envelope);

	if (result == ShipResult::Failed) {
		// Back-off/retry accounting: re-queue the batch's events at the FRONT so
		// the next flush retries them, bounded so a dead endpoint can't grow
		// memory without limit. The actual back-off delay is the transport's
		// concern (it runs async). Drops summary is preserved.
		std::vector<LogEvent> requeued = batch.events;
		if (requeued.size() + buffer_.size() > requeue_cap()) {
			// Keep the most recent events within the cap (drop oldest).
			const std::size_t keep = requeue_cap() > buffer_.size()
			                             ? requeue_cap() - buffer_.size()
			                             : 0;
			if (keep < requeued.size()) {
				requeued.erase(requeued.begin(),
				               requeued.begin() +
				                   static_cast<std::ptrdiff_t>(requeued.size() - keep));
			}
		}
		buffer_.insert(buffer_.begin(), requeued.begin(), requeued.end());
		if (!buffer_.empty() && !have_first_) {
			have_first_        = true;
			first_buffered_ms_ = batch.events.empty() ? 0 : batch.events.front().timestamp_ms;
		}
		dropped_by_rate_limit_ += batch.dropped_by_rate_limit;
	}
	return result;
}

bool LogCapture::flush_and_ship(ITransport &transport, uint64_t now_ms) {
	if (!flush_due(now_ms)) {
		return false;
	}
	LogBatch batch = flush();
	ship(batch, transport);
	return true;
}

} // namespace meridian::telemetry
